// Reads the machine's hardware description from /proc and /sys and flags
// problems. Every parser takes a root path so tests can use a fake tree.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/affinity.hpp"
#include "common/json.hpp"

namespace hwdiag::inventory {

struct CpuInfo {
  std::string vendor;
  std::string model;
  int logical_cpus = 0;
  std::vector<std::string> flags;
};
CpuInfo parse_cpuinfo(const std::string& root = "/");

struct Topology {
  int sockets = 0;
  int cores = 0;
  int threads = 0;
  double threads_per_core = 0.0;
  std::vector<CpuTopo> cpus;
};
Topology parse_topology(const std::string& root = "/");

struct CacheInfo {
  int level = 0;
  std::string type;  // Data, Instruction, Unified
  uint64_t size_bytes = 0;
  int line_size = 0;
  std::string shared_cpu_list;
};
std::vector<CacheInfo> parse_caches(const std::string& root = "/", int cpu = 0);

// The largest Data or Unified cache: the last-level cache.
std::optional<CacheInfo> last_level_cache(const std::vector<CacheInfo>& caches);

struct CpuFreq {
  int cpu = 0;
  std::optional<double> cur_mhz;
  std::optional<double> min_mhz;
  std::optional<double> max_mhz;
};
std::vector<CpuFreq> parse_cpufreq(const std::string& root = "/");

struct MemInfo {
  uint64_t total_kib = 0;
  uint64_t free_kib = 0;
  uint64_t available_kib = 0;
  uint64_t hugepages_total = 0;
  uint64_t hugepage_size_kib = 0;
};
MemInfo parse_meminfo(const std::string& root = "/");

struct NumaNode {
  int id = 0;
  std::vector<int> cpus;
  std::optional<uint64_t> mem_total_kib;
};
std::vector<NumaNode> parse_numa(const std::string& root = "/");

struct PcieLink {
  std::optional<double> cur_speed_gts;  // nullopt when "Unknown"
  std::optional<double> max_speed_gts;
  int cur_width = 0;
  int max_width = 0;
  std::string cur_speed_raw;
  std::string max_speed_raw;
};

struct PciDevice {
  std::string address;
  std::string vendor;
  std::string device;
  std::string class_code;
  std::string class_name;
  std::optional<PcieLink> link;  // nullopt when the device has no link attributes
};
std::vector<PciDevice> parse_pci(const std::string& root = "/");

// "8.0 GT/s PCIe" -> 8.0, "2.5 GT/s" -> 2.5, "Unknown" -> nullopt.
std::optional<double> parse_pcie_speed(const std::string& s);
std::string pci_class_name(const std::string& class_code);

struct Sensor {
  std::string chip;   // hwmon "name"
  std::string label;  // temp1_label, or "temp1" when there is none
  std::string kind;   // "temperature" or "voltage"
  double value = 0.0; // degrees C or volts (sysfs gives milli-units)
  std::string unit;
};
std::vector<Sensor> parse_hwmon(const std::string& root = "/");

struct EdacController {
  std::string name;
  uint64_t ce_count = 0;
  uint64_t ue_count = 0;
};
// nullopt when there is no EDAC driver (normal on desktops and VMs).
std::optional<std::vector<EdacController>> parse_edac(const std::string& root = "/");

enum class Level { Info, Ok, Warn, Fail };
const char* level_name(Level l);

struct Check {
  Level level = Level::Ok;
  std::string check;
  std::string subject;
  std::string message;
};

struct Inventory {
  CpuInfo cpu;
  Topology topology;
  std::vector<CacheInfo> caches;
  std::vector<CpuFreq> freq;
  std::optional<std::string> governor;
  MemInfo memory;
  std::vector<NumaNode> numa;
  std::vector<PciDevice> pci;
  std::vector<Sensor> sensors;
  std::optional<std::vector<EdacController>> edac;
};

Inventory collect(const std::string& root = "/");

std::vector<Check> run_checks(const Inventory& inv, double temp_threshold_c = 85.0);

// "fail" if any check fails, else "warn" if any warns, else "ok".
std::string overall_status(const std::vector<Check>& checks);

json::Value to_json(const Inventory& inv);
json::Value to_json(const std::vector<Check>& checks);

}  // namespace hwdiag::inventory
