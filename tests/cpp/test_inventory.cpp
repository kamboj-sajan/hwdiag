#include <gtest/gtest.h>

#include <algorithm>

#include "common/hostinfo.hpp"
#include "inventory/inventory.hpp"

using namespace hwdiag;
using namespace hwdiag::inventory;

namespace {

const std::string kDesktop = std::string(HWDIAG_FIXTURES_DIR) + "/sys/desktop";
const std::string kServer = std::string(HWDIAG_FIXTURES_DIR) + "/sys/server";

std::vector<Check> of_level(const std::vector<Check>& checks, Level level) {
  std::vector<Check> out;
  std::copy_if(checks.begin(), checks.end(), std::back_inserter(out),
               [level](const Check& c) { return c.level == level; });
  return out;
}

}  // namespace

TEST(Inventory, DesktopCpuInfo) {
  const CpuInfo cpu = parse_cpuinfo(kDesktop);
  EXPECT_EQ(cpu.model, "Intel(R) Core(TM) i7-7700K CPU @ 4.20GHz");
  EXPECT_EQ(cpu.vendor, "GenuineIntel");
  EXPECT_EQ(cpu.logical_cpus, 8);
  EXPECT_NE(std::find(cpu.flags.begin(), cpu.flags.end(), "avx2"), cpu.flags.end());
}

TEST(Inventory, DesktopTopologyHasSmt) {
  const Topology t = parse_topology(kDesktop);
  EXPECT_EQ(t.sockets, 1);
  EXPECT_EQ(t.cores, 4);
  EXPECT_EQ(t.threads, 8);
  EXPECT_DOUBLE_EQ(t.threads_per_core, 2.0);
}

TEST(Inventory, ServerTopologyHasTwoSocketsNoSmt) {
  const Topology t = parse_topology(kServer);
  EXPECT_EQ(t.sockets, 2);
  EXPECT_EQ(t.cores, 8);
  EXPECT_EQ(t.threads, 8);
  EXPECT_DOUBLE_EQ(t.threads_per_core, 1.0);
}

TEST(Inventory, DesktopCaches) {
  const auto caches = parse_caches(kDesktop);
  ASSERT_EQ(caches.size(), 4u);
  EXPECT_EQ(caches[0].level, 1);
  EXPECT_EQ(caches[0].type, "Data");
  EXPECT_EQ(caches[0].size_bytes, 32u * 1024);
  EXPECT_EQ(caches[0].line_size, 64);
  EXPECT_EQ(caches[2].size_bytes, 256u * 1024);
  const auto llc = last_level_cache(caches);
  ASSERT_TRUE(llc.has_value());
  EXPECT_EQ(llc->level, 3);
  EXPECT_EQ(llc->size_bytes, 8192u * 1024);
}

TEST(Inventory, DesktopFrequencyInMhz) {
  const auto f = parse_cpufreq(kDesktop);
  ASSERT_EQ(f.size(), 8u);
  EXPECT_DOUBLE_EQ(*f[0].cur_mhz, 4400.0);
  EXPECT_DOUBLE_EQ(*f[7].cur_mhz, 3700.0);
  EXPECT_DOUBLE_EQ(*f[0].max_mhz, 4500.0);
}

TEST(Inventory, ServerHasNoCpufreq) { EXPECT_TRUE(parse_cpufreq(kServer).empty()); }

TEST(Inventory, MemInfoInKib) {
  const MemInfo m = parse_meminfo(kDesktop);
  EXPECT_EQ(m.total_kib, 32768000u);
  EXPECT_EQ(m.available_kib, 28000000u);
  EXPECT_EQ(m.hugepage_size_kib, 2048u);
  EXPECT_EQ(parse_meminfo(kServer).hugepages_total, 16u);
}

TEST(Inventory, ServerHasTwoNumaNodes) {
  const auto nodes = parse_numa(kServer);
  ASSERT_EQ(nodes.size(), 2u);
  EXPECT_EQ(nodes[1].id, 1);
  EXPECT_EQ(nodes[1].cpus, (std::vector<int>{4, 5, 6, 7}));
  EXPECT_EQ(nodes[1].mem_total_kib, 131960796u);
}

TEST(Inventory, PcieSpeedParsing) {
  EXPECT_DOUBLE_EQ(*parse_pcie_speed("8.0 GT/s PCIe"), 8.0);
  EXPECT_DOUBLE_EQ(*parse_pcie_speed("2.5 GT/s"), 2.5);
  EXPECT_DOUBLE_EQ(*parse_pcie_speed("16.0 GT/s PCIe\n"), 16.0);
  EXPECT_FALSE(parse_pcie_speed("Unknown").has_value());
  EXPECT_FALSE(parse_pcie_speed("").has_value());
}

TEST(Inventory, PciClassNames) {
  EXPECT_EQ(pci_class_name("0x030000"), "VGA controller");
  EXPECT_EQ(pci_class_name("0x010802"), "NVMe controller");
  EXPECT_EQ(pci_class_name("0xff0000"), "class 0xff00");
}

TEST(Inventory, DesktopPciDevicesAndLinks) {
  const auto devs = parse_pci(kDesktop);
  ASSERT_EQ(devs.size(), 4u);
  EXPECT_EQ(devs[0].address, "0000:00:00.0");
  EXPECT_FALSE(devs[0].link.has_value());  // host bridge: no link attributes
  ASSERT_TRUE(devs[1].link.has_value());
  EXPECT_EQ(devs[1].vendor, "0x10de");
  EXPECT_DOUBLE_EQ(*devs[1].link->cur_speed_gts, 2.5);
  EXPECT_DOUBLE_EQ(*devs[1].link->max_speed_gts, 16.0);
  EXPECT_EQ(devs[1].link->cur_width, 16);
  ASSERT_TRUE(devs[3].link.has_value());
  EXPECT_FALSE(devs[3].link->cur_speed_gts.has_value());  // "Unknown"
  EXPECT_EQ(devs[3].link->cur_width, 0);
}

TEST(Inventory, HwmonConvertsMilliUnits) {
  const auto sensors = parse_hwmon(kDesktop);
  ASSERT_EQ(sensors.size(), 4u);
  EXPECT_EQ(sensors[0].chip, "coretemp");
  EXPECT_EQ(sensors[0].label, "Package id 0");
  EXPECT_DOUBLE_EQ(sensors[0].value, 91.0);
  EXPECT_EQ(sensors[2].chip, "nvme");
  EXPECT_DOUBLE_EQ(sensors[2].value, 42.85);
  EXPECT_EQ(sensors[3].kind, "voltage");
  EXPECT_EQ(sensors[3].label, "in0");
  EXPECT_DOUBLE_EQ(sensors[3].value, 1.032);
}

TEST(Inventory, EdacPresentOnlyOnServer) {
  EXPECT_FALSE(parse_edac(kDesktop).has_value());
  const auto edac = parse_edac(kServer);
  ASSERT_TRUE(edac.has_value());
  ASSERT_EQ(edac->size(), 2u);
  EXPECT_EQ((*edac)[0].ce_count, 3u);
  EXPECT_EQ((*edac)[0].ue_count, 0u);
}

TEST(Inventory, DesktopChecksWarnOnSlowGpuLinkAndHotSensor) {
  const auto checks = run_checks(collect(kDesktop), 85.0);
  const auto warns = of_level(checks, Level::Warn);
  ASSERT_EQ(warns.size(), 2u);
  EXPECT_EQ(warns[0].check, "pcie_link");
  EXPECT_EQ(warns[0].subject, "0000:01:00.0");
  EXPECT_NE(warns[0].message.find("re-check under load"), std::string::npos);
  EXPECT_NE(warns[0].message.find("2.5 GT/s x16"), std::string::npos);
  EXPECT_EQ(warns[1].check, "temperature");
  EXPECT_EQ(warns[1].subject, "coretemp/Package id 0");
  EXPECT_TRUE(of_level(checks, Level::Fail).empty());
  // No EDAC on a desktop is a note, not a warning.
  const auto infos = of_level(checks, Level::Info);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].check, "edac");
  EXPECT_EQ(overall_status(checks), "warn");
}

TEST(Inventory, ServerChecksFailOnCorrectedErrorsAndWarnOnNarrowLink) {
  const auto checks = run_checks(collect(kServer), 85.0);
  const auto fails = of_level(checks, Level::Fail);
  ASSERT_EQ(fails.size(), 1u);
  EXPECT_EQ(fails[0].subject, "mc0");
  EXPECT_NE(fails[0].message.find("3 corrected"), std::string::npos);
  const auto warns = of_level(checks, Level::Warn);
  ASSERT_EQ(warns.size(), 1u);
  EXPECT_EQ(warns[0].subject, "0000:17:00.0");
  EXPECT_NE(warns[0].message.find("narrower"), std::string::npos);
  EXPECT_EQ(overall_status(checks), "fail");
}

TEST(Inventory, TemperatureThresholdIsConfigurable) {
  const auto checks = run_checks(collect(kDesktop), 95.0);
  for (const auto& c : checks) EXPECT_FALSE(c.check == "temperature" && c.level == Level::Warn);
}

TEST(HostInfo, ReadsModelTurboAndSmtFromRoot) {
  EXPECT_EQ(read_cpu_model(kDesktop), "Intel(R) Core(TM) i7-7700K CPU @ 4.20GHz");
  EXPECT_EQ(read_turbo(kDesktop), "on");
  EXPECT_EQ(read_smt(kDesktop), "on");
  EXPECT_EQ(read_smt(kServer), "off");
  EXPECT_EQ(read_turbo(kServer), "unknown");
  const HostInfo h = collect_host_info(kDesktop);
  EXPECT_EQ(h.governor.value_or(""), "powersave");
  EXPECT_EQ(h.logical_cpus, 8);
  EXPECT_FALSE(collect_host_info(kServer).governor.has_value());
}

TEST(Inventory, JsonHasEveryTopLevelSection) {
  const auto j = to_json(collect(kServer));
  for (const char* key : {"cpu", "topology", "caches", "frequency", "memory", "numa", "pcie", "sensors", "edac"}) {
    EXPECT_NE(j.find(key), nullptr) << key;
  }
}
