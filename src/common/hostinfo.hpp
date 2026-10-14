#pragma once

#include <optional>
#include <string>

#include "common/json.hpp"

namespace hwdiag {

// The "host" block of every result: what was measured, and the settings
// that change the numbers (governor, turbo, SMT, compiler flags).
struct HostInfo {
  std::string hostname;
  std::string kernel;
  std::string cpu_model;
  int logical_cpus = 0;
  std::optional<std::string> governor;  // nullopt when there is no cpufreq
  std::string turbo = "unknown";        // on / off / unknown
  std::string smt = "unknown";          // on / off / unknown
  std::string compiler;
  std::string compiler_flags;
  std::string build_type;
};

HostInfo collect_host_info(const std::string& root = "/");

// The CPU model string from /proc/cpuinfo ("model name" on x86; falls back
// to other fields on ARM). "unknown" if nothing matches.
std::string read_cpu_model(const std::string& root = "/");

// Turbo state from intel_pstate/no_turbo or cpufreq/boost.
std::string read_turbo(const std::string& root = "/");

// SMT state from /sys/devices/system/cpu/smt/active.
std::string read_smt(const std::string& root = "/");

json::Value to_json(const HostInfo& h);

}  // namespace hwdiag
