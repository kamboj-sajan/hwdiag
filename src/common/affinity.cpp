#include "common/affinity.hpp"

#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <tuple>
#include <unistd.h>

#include "common/sysfs.hpp"

namespace hwdiag {

std::vector<int> allowed_cpus() {
  std::vector<int> cpus;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) == 0) {
    for (int c = 0; c < CPU_SETSIZE; ++c) {
      if (CPU_ISSET(c, &set)) cpus.push_back(c);
    }
  }
  if (cpus.empty()) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long c = 0; c < std::max(1L, n); ++c) cpus.push_back(static_cast<int>(c));
  }
  return cpus;
}

bool pin_thread(int cpu, std::string* err) {
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    if (err) *err = "cpu " + std::to_string(cpu) + " out of range";
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  // pid 0 means the calling thread, not the whole process.
  if (sched_setaffinity(0, sizeof set, &set) != 0) {
    if (err) *err = "sched_setaffinity(cpu " + std::to_string(cpu) + "): " + std::strerror(errno);
    return false;
  }
  return true;
}

std::vector<CpuTopo> read_cpu_topology(const std::vector<int>& cpus, const std::string& root) {
  std::vector<CpuTopo> out;
  for (int cpu : cpus) {
    const std::string dir =
        sysfs::under(root, "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/");
    CpuTopo t;
    t.cpu = cpu;
    t.package = static_cast<int>(sysfs::read_int(dir + "physical_package_id").value_or(0));
    t.die = static_cast<int>(sysfs::read_int(dir + "die_id").value_or(0));
    t.core = static_cast<int>(sysfs::read_int(dir + "core_id").value_or(cpu));
    out.push_back(t);
  }
  return out;
}

std::vector<int> spread_order(const std::vector<CpuTopo>& topo) {
  // Group CPUs by physical core, keeping the CPU order inside each core.
  std::map<std::tuple<int, int, int>, std::vector<int>> by_core;
  std::vector<std::tuple<int, int, int>> core_order;
  for (const auto& t : topo) {
    auto key = std::make_tuple(t.package, t.die, t.core);
    auto [it, inserted] = by_core.try_emplace(key);
    if (inserted) core_order.push_back(key);
    it->second.push_back(t.cpu);
  }
  std::vector<int> order;
  for (size_t round = 0; order.size() < topo.size(); ++round) {
    for (const auto& key : core_order) {
      const auto& siblings = by_core[key];
      if (round < siblings.size()) order.push_back(siblings[round]);
    }
  }
  return order;
}

}  // namespace hwdiag
