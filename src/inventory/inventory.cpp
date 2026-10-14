#include "inventory/inventory.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "common/sysfs.hpp"

namespace hwdiag::inventory {

using sysfs::under;

namespace {

std::optional<std::string> opt_text(const std::string& path) {
  auto t = sysfs::read_text(path);
  if (!t || t->empty()) return std::nullopt;
  return t;
}

std::string fmt(const char* f, double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, f, v);
  return buf;
}

}  // namespace

CpuInfo parse_cpuinfo(const std::string& root) {
  CpuInfo info;
  std::ifstream in(under(root, "/proc/cpuinfo"));
  std::string line;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = sysfs::trim(line.substr(0, colon));
    const std::string value = sysfs::trim(line.substr(colon + 1));
    if (key == "processor") {
      ++info.logical_cpus;
    } else if (key == "vendor_id" && info.vendor.empty()) {
      info.vendor = value;
    } else if ((key == "model name" || key == "Model") && info.model.empty()) {
      info.model = value;
    } else if ((key == "flags" || key == "Features") && info.flags.empty()) {
      std::istringstream ss(value);
      std::string f;
      while (ss >> f) info.flags.push_back(f);
    }
  }
  if (info.model.empty()) info.model = "unknown";
  return info;
}

Topology parse_topology(const std::string& root) {
  Topology t;
  std::vector<int> cpus;
  for (const auto& name : sysfs::list_dir(under(root, "/sys/devices/system/cpu"), "cpu")) {
    if (auto n = sysfs::numeric_suffix(name, "cpu")) cpus.push_back(*n);
  }
  std::sort(cpus.begin(), cpus.end());
  t.cpus = read_cpu_topology(cpus, root);
  std::set<int> packages;
  std::set<std::tuple<int, int, int>> cores;
  for (const auto& c : t.cpus) {
    packages.insert(c.package);
    cores.insert({c.package, c.die, c.core});
  }
  t.sockets = static_cast<int>(packages.size());
  t.cores = static_cast<int>(cores.size());
  t.threads = static_cast<int>(t.cpus.size());
  t.threads_per_core = t.cores ? static_cast<double>(t.threads) / t.cores : 0.0;
  return t;
}

std::vector<CacheInfo> parse_caches(const std::string& root, int cpu) {
  std::vector<CacheInfo> caches;
  const std::string dir = under(root, "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache");
  for (const auto& name : sysfs::list_dir(dir, "index")) {
    const std::string base = dir + "/" + name + "/";
    CacheInfo c;
    c.level = static_cast<int>(sysfs::read_int(base + "level").value_or(0));
    c.type = sysfs::read_text(base + "type").value_or("");
    c.size_bytes = sysfs::parse_size(sysfs::read_text(base + "size").value_or("")).value_or(0);
    c.line_size = static_cast<int>(sysfs::read_int(base + "coherency_line_size").value_or(0));
    c.shared_cpu_list = sysfs::read_text(base + "shared_cpu_list").value_or("");
    if (c.level > 0) caches.push_back(c);
  }
  std::sort(caches.begin(), caches.end(), [](const CacheInfo& a, const CacheInfo& b) {
    return std::tie(a.level, a.type) < std::tie(b.level, b.type);
  });
  return caches;
}

std::optional<CacheInfo> last_level_cache(const std::vector<CacheInfo>& caches) {
  std::optional<CacheInfo> best;
  for (const auto& c : caches) {
    if (c.type == "Instruction") continue;
    if (!best || c.level > best->level) best = c;
  }
  return best;
}

std::vector<CpuFreq> parse_cpufreq(const std::string& root) {
  std::vector<CpuFreq> out;
  for (const auto& name : sysfs::list_dir(under(root, "/sys/devices/system/cpu"), "cpu")) {
    auto n = sysfs::numeric_suffix(name, "cpu");
    if (!n) continue;
    const std::string base = under(root, "/sys/devices/system/cpu/" + name + "/cpufreq/");
    CpuFreq f;
    f.cpu = *n;
    // sysfs reports kHz.
    if (auto v = sysfs::read_double(base + "scaling_cur_freq")) f.cur_mhz = *v / 1000.0;
    if (auto v = sysfs::read_double(base + "cpuinfo_min_freq")) f.min_mhz = *v / 1000.0;
    if (auto v = sysfs::read_double(base + "cpuinfo_max_freq")) f.max_mhz = *v / 1000.0;
    if (f.cur_mhz || f.min_mhz || f.max_mhz) out.push_back(f);
  }
  return out;
}

MemInfo parse_meminfo(const std::string& root) {
  MemInfo m;
  std::ifstream in(under(root, "/proc/meminfo"));
  std::string line;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, colon);
    unsigned long long v = 0;
    if (std::sscanf(line.c_str() + colon + 1, "%llu", &v) != 1) continue;
    if (key == "MemTotal") m.total_kib = v;
    else if (key == "MemFree") m.free_kib = v;
    else if (key == "MemAvailable") m.available_kib = v;
    else if (key == "HugePages_Total") m.hugepages_total = v;
    else if (key == "Hugepagesize") m.hugepage_size_kib = v;
  }
  return m;
}

std::vector<NumaNode> parse_numa(const std::string& root) {
  std::vector<NumaNode> nodes;
  const std::string dir = under(root, "/sys/devices/system/node");
  for (const auto& name : sysfs::list_dir(dir, "node")) {
    auto id = sysfs::numeric_suffix(name, "node");
    if (!id) continue;
    NumaNode n;
    n.id = *id;
    n.cpus = sysfs::parse_cpu_list(sysfs::read_text(dir + "/" + name + "/cpulist").value_or(""));
    // Lines look like "Node 0 MemTotal:       16303440 kB".
    std::ifstream in(dir + "/" + name + "/meminfo");
    std::string line;
    while (std::getline(in, line)) {
      const auto pos = line.find("MemTotal:");
      unsigned long long v = 0;
      if (pos != std::string::npos && std::sscanf(line.c_str() + pos + 9, "%llu", &v) == 1) {
        n.mem_total_kib = v;
      }
    }
    nodes.push_back(n);
  }
  return nodes;
}

std::optional<double> parse_pcie_speed(const std::string& s) {
  const std::string t = sysfs::trim(s);
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str() || v <= 0.0) return std::nullopt;
  return v;
}

std::string pci_class_name(const std::string& class_code) {
  static const std::map<unsigned, const char*> names = {
      {0x0100, "SCSI controller"},   {0x0101, "IDE controller"},     {0x0104, "RAID controller"},
      {0x0106, "SATA controller"},   {0x0108, "NVMe controller"},    {0x0200, "Ethernet controller"},
      {0x0280, "Network controller"}, {0x0300, "VGA controller"},    {0x0302, "3D controller"},
      {0x0380, "Display controller"}, {0x0403, "Audio device"},      {0x0600, "Host bridge"},
      {0x0601, "ISA bridge"},        {0x0604, "PCI bridge"},         {0x0780, "Communication controller"},
      {0x0880, "System peripheral"}, {0x0c03, "USB controller"},     {0x0c05, "SMBus"},
      {0x1180, "Signal processing controller"}, {0x1200, "Processing accelerator"},
  };
  const unsigned long code = std::strtoul(class_code.c_str(), nullptr, 16);
  const unsigned key = static_cast<unsigned>(code >> 8);
  if (auto it = names.find(key); it != names.end()) return it->second;
  char buf[32];
  std::snprintf(buf, sizeof buf, "class 0x%04x", key);
  return buf;
}

std::vector<PciDevice> parse_pci(const std::string& root) {
  std::vector<PciDevice> out;
  const std::string dir = under(root, "/sys/bus/pci/devices");
  for (const auto& name : sysfs::list_dir(dir)) {
    const std::string base = dir + "/" + name + "/";
    PciDevice d;
    d.address = name;
    d.vendor = sysfs::read_text(base + "vendor").value_or("");
    d.device = sysfs::read_text(base + "device").value_or("");
    d.class_code = sysfs::read_text(base + "class").value_or("");
    d.class_name = d.class_code.empty() ? "unknown" : pci_class_name(d.class_code);
    auto cur_speed = opt_text(base + "current_link_speed");
    auto max_speed = opt_text(base + "max_link_speed");
    auto cur_width = sysfs::read_int(base + "current_link_width");
    auto max_width = sysfs::read_int(base + "max_link_width");
    if (cur_speed && max_speed && cur_width && max_width) {
      PcieLink l;
      l.cur_speed_raw = *cur_speed;
      l.max_speed_raw = *max_speed;
      l.cur_speed_gts = parse_pcie_speed(*cur_speed);
      l.max_speed_gts = parse_pcie_speed(*max_speed);
      l.cur_width = static_cast<int>(*cur_width);
      l.max_width = static_cast<int>(*max_width);
      d.link = l;
    }
    out.push_back(d);
  }
  return out;
}

std::vector<Sensor> parse_hwmon(const std::string& root) {
  std::vector<Sensor> out;
  const std::string dir = under(root, "/sys/class/hwmon");
  for (const auto& hw : sysfs::list_dir(dir, "hwmon")) {
    const std::string base = dir + "/" + hw + "/";
    const std::string chip = sysfs::read_text(base + "name").value_or(hw);
    for (const auto& file : sysfs::list_dir(base)) {
      const bool temp = file.rfind("temp", 0) == 0;
      const bool volt = file.rfind("in", 0) == 0;
      const auto suffix = file.find("_input");
      if ((!temp && !volt) || suffix == std::string::npos || suffix + 6 != file.size()) continue;
      const std::string stem = file.substr(0, suffix);  // "temp1" or "in0"
      auto raw = sysfs::read_double(base + file);
      if (!raw) continue;
      Sensor s;
      s.chip = chip;
      s.label = sysfs::read_text(base + stem + "_label").value_or(stem);
      s.kind = temp ? "temperature" : "voltage";
      s.value = *raw / 1000.0;  // millidegrees C or millivolts
      s.unit = temp ? "C" : "V";
      out.push_back(s);
    }
  }
  return out;
}

std::optional<std::vector<EdacController>> parse_edac(const std::string& root) {
  const std::string dir = under(root, "/sys/devices/system/edac/mc");
  auto names = sysfs::list_dir(dir, "mc");
  std::vector<EdacController> out;
  for (const auto& name : names) {
    if (!sysfs::numeric_suffix(name, "mc")) continue;
    EdacController c;
    c.name = name;
    c.ce_count = static_cast<uint64_t>(sysfs::read_int(dir + "/" + name + "/ce_count").value_or(0));
    c.ue_count = static_cast<uint64_t>(sysfs::read_int(dir + "/" + name + "/ue_count").value_or(0));
    out.push_back(c);
  }
  if (out.empty()) return std::nullopt;
  return out;
}

const char* level_name(Level l) {
  switch (l) {
    case Level::Info: return "info";
    case Level::Ok: return "ok";
    case Level::Warn: return "warn";
    case Level::Fail: return "fail";
  }
  return "unknown";
}

Inventory collect(const std::string& root) {
  Inventory inv;
  inv.cpu = parse_cpuinfo(root);
  inv.topology = parse_topology(root);
  inv.caches = parse_caches(root, inv.topology.cpus.empty() ? 0 : inv.topology.cpus.front().cpu);
  inv.freq = parse_cpufreq(root);
  if (auto g = opt_text(under(root, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))) inv.governor = g;
  inv.memory = parse_meminfo(root);
  inv.numa = parse_numa(root);
  inv.pci = parse_pci(root);
  inv.sensors = parse_hwmon(root);
  inv.edac = parse_edac(root);
  return inv;
}

std::vector<Check> run_checks(const Inventory& inv, double temp_threshold_c) {
  std::vector<Check> checks;

  // PCIe links. Devices without link attributes, with an "Unknown" speed or
  // a width of 0 (link down) can't be judged and are skipped.
  int links_checked = 0, links_slow = 0;
  for (const auto& d : inv.pci) {
    if (!d.link) continue;
    const auto& l = *d.link;
    if (!l.cur_speed_gts || !l.max_speed_gts || l.cur_width == 0 || l.max_width == 0) continue;
    ++links_checked;
    const bool slow = *l.cur_speed_gts + 1e-9 < *l.max_speed_gts;
    const bool narrow = l.cur_width < l.max_width;
    if (!slow && !narrow) continue;
    ++links_slow;
    std::string msg = d.class_name + " link at " + fmt("%g", *l.cur_speed_gts) + " GT/s x" +
                      std::to_string(l.cur_width) + ", maximum " + fmt("%g", *l.max_speed_gts) + " GT/s x" +
                      std::to_string(l.max_width) + " (" + (slow && narrow ? "slower and narrower" : slow ? "slower" : "narrower") +
                      "). GPUs and some other devices slow their links on purpose when idle: re-check under load";
    checks.push_back({Level::Warn, "pcie_link", d.address, msg});
  }
  if (links_checked == 0) {
    checks.push_back({Level::Info, "pcie_link", "all", "no PCIe devices report link speed and width (normal in VMs)"});
  } else if (links_slow == 0) {
    checks.push_back({Level::Ok, "pcie_link", "all",
                      std::to_string(links_checked) + " PCIe link(s) at maximum speed and width"});
  }

  // Temperatures (hwmon reports millidegrees; parse_hwmon already converted).
  int temps = 0, hot = 0;
  double hottest = -1e9;
  for (const auto& s : inv.sensors) {
    if (s.kind != "temperature") continue;
    ++temps;
    hottest = std::max(hottest, s.value);
    if (s.value > temp_threshold_c) {
      ++hot;
      checks.push_back({Level::Warn, "temperature", s.chip + "/" + s.label,
                        fmt("%.1f C", s.value) + " is above the " + fmt("%.0f C", temp_threshold_c) + " threshold"});
    }
  }
  if (temps == 0) {
    checks.push_back({Level::Info, "temperature", "hwmon", "no temperature sensors found (normal in VMs)"});
  } else if (hot == 0) {
    checks.push_back({Level::Ok, "temperature", "all",
                      std::to_string(temps) + " sensor(s), hottest " + fmt("%.1f C", hottest)});
  }

  // Memory error counters. Any corrected or uncorrected error is a fail.
  if (!inv.edac) {
    checks.push_back({Level::Info, "edac", "edac",
                      "EDAC not present (normal on desktops and VMs); memory error counts can't be read"});
  } else {
    bool any = false;
    for (const auto& c : *inv.edac) {
      if (c.ce_count == 0 && c.ue_count == 0) continue;
      any = true;
      checks.push_back({Level::Fail, "edac", c.name,
                        std::to_string(c.ce_count) + " corrected and " + std::to_string(c.ue_count) +
                            " uncorrected memory error(s)"});
    }
    if (!any) {
      checks.push_back({Level::Ok, "edac", "all",
                        "0 memory errors on " + std::to_string(inv.edac->size()) + " controller(s)"});
    }
  }
  return checks;
}

std::string overall_status(const std::vector<Check>& checks) {
  bool warn = false;
  for (const auto& c : checks) {
    if (c.level == Level::Fail) return "fail";
    if (c.level == Level::Warn) warn = true;
  }
  return warn ? "warn" : "ok";
}

namespace {

json::Value opt(const std::optional<double>& v) { return v ? json::Value(*v) : json::Value(nullptr); }

}  // namespace

json::Value to_json(const Inventory& inv) {
  auto r = json::Value::object();

  auto cpu = json::Value::object();
  cpu["vendor"] = inv.cpu.vendor;
  cpu["model"] = inv.cpu.model;
  cpu["logical_cpus"] = inv.cpu.logical_cpus;
  auto flags = json::Value::array();
  for (const auto& f : inv.cpu.flags) flags.push_back(f);
  cpu["flags"] = std::move(flags);
  r["cpu"] = std::move(cpu);

  auto topo = json::Value::object();
  topo["sockets"] = inv.topology.sockets;
  topo["cores"] = inv.topology.cores;
  topo["threads"] = inv.topology.threads;
  topo["threads_per_core"] = inv.topology.threads_per_core;
  r["topology"] = std::move(topo);

  auto caches = json::Value::array();
  for (const auto& c : inv.caches) {
    auto v = json::Value::object();
    v["level"] = c.level;
    v["type"] = c.type;
    v["size_bytes"] = c.size_bytes;
    v["line_size"] = c.line_size;
    v["shared_cpu_list"] = c.shared_cpu_list;
    caches.push_back(std::move(v));
  }
  r["caches"] = std::move(caches);

  auto freq = json::Value::object();
  freq["available"] = !inv.freq.empty();
  freq["governor"] = inv.governor ? json::Value(*inv.governor) : json::Value(nullptr);
  auto per_cpu = json::Value::array();
  for (const auto& f : inv.freq) {
    auto v = json::Value::object();
    v["cpu"] = f.cpu;
    v["cur_mhz"] = opt(f.cur_mhz);
    v["min_mhz"] = opt(f.min_mhz);
    v["max_mhz"] = opt(f.max_mhz);
    per_cpu.push_back(std::move(v));
  }
  freq["per_cpu"] = std::move(per_cpu);
  r["frequency"] = std::move(freq);

  auto mem = json::Value::object();
  mem["total_kib"] = inv.memory.total_kib;
  mem["free_kib"] = inv.memory.free_kib;
  mem["available_kib"] = inv.memory.available_kib;
  mem["hugepages_total"] = inv.memory.hugepages_total;
  mem["hugepage_size_kib"] = inv.memory.hugepage_size_kib;
  r["memory"] = std::move(mem);

  auto numa = json::Value::array();
  for (const auto& n : inv.numa) {
    auto v = json::Value::object();
    v["node"] = n.id;
    auto cpus = json::Value::array();
    for (int c : n.cpus) cpus.push_back(c);
    v["cpus"] = std::move(cpus);
    v["mem_total_kib"] = n.mem_total_kib ? json::Value(*n.mem_total_kib) : json::Value(nullptr);
    numa.push_back(std::move(v));
  }
  r["numa"] = std::move(numa);

  auto pci = json::Value::array();
  for (const auto& d : inv.pci) {
    auto v = json::Value::object();
    v["address"] = d.address;
    v["vendor"] = d.vendor;
    v["device"] = d.device;
    v["class"] = d.class_code;
    v["class_name"] = d.class_name;
    if (d.link) {
      auto l = json::Value::object();
      l["cur_speed_gts"] = opt(d.link->cur_speed_gts);
      l["cur_width"] = d.link->cur_width;
      l["max_speed_gts"] = opt(d.link->max_speed_gts);
      l["max_width"] = d.link->max_width;
      l["cur_speed_raw"] = d.link->cur_speed_raw;
      l["max_speed_raw"] = d.link->max_speed_raw;
      v["link"] = std::move(l);
    } else {
      v["link"] = nullptr;
    }
    pci.push_back(std::move(v));
  }
  r["pcie"] = std::move(pci);

  auto sensors = json::Value::array();
  for (const auto& s : inv.sensors) {
    auto v = json::Value::object();
    v["chip"] = s.chip;
    v["label"] = s.label;
    v["kind"] = s.kind;
    v["value"] = s.value;
    v["unit"] = s.unit;
    sensors.push_back(std::move(v));
  }
  r["sensors"] = std::move(sensors);

  auto edac = json::Value::object();
  edac["present"] = inv.edac.has_value();
  auto ctrls = json::Value::array();
  if (inv.edac) {
    for (const auto& c : *inv.edac) {
      auto v = json::Value::object();
      v["name"] = c.name;
      v["ce_count"] = c.ce_count;
      v["ue_count"] = c.ue_count;
      ctrls.push_back(std::move(v));
    }
  }
  edac["controllers"] = std::move(ctrls);
  r["edac"] = std::move(edac);
  return r;
}

json::Value to_json(const std::vector<Check>& checks) {
  auto arr = json::Value::array();
  for (const auto& c : checks) {
    auto v = json::Value::object();
    v["level"] = level_name(c.level);
    v["check"] = c.check;
    v["subject"] = c.subject;
    v["message"] = c.message;
    arr.push_back(std::move(v));
  }
  return arr;
}

}  // namespace hwdiag::inventory
