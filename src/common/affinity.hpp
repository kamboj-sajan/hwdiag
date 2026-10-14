#pragma once

#include <string>
#include <vector>

namespace hwdiag {

// CPUs this process may run on (sched_getaffinity), in ascending order.
std::vector<int> allowed_cpus();

// Pins the calling thread to one CPU. Returns false and fills err on failure.
bool pin_thread(int cpu, std::string* err = nullptr);

struct CpuTopo {
  int cpu = 0;
  int package = 0;
  int die = 0;
  int core = 0;
};

// Topology of the given CPUs from /sys/devices/system/cpu/cpuN/topology/.
// CPUs without topology files are reported as their own core.
std::vector<CpuTopo> read_cpu_topology(const std::vector<int>& cpus, const std::string& root = "/");

// Orders CPUs so that threads land on separate physical cores before any
// core gets a second SMT thread: first CPU of each core, then the second, ...
std::vector<int> spread_order(const std::vector<CpuTopo>& topo);

}  // namespace hwdiag
