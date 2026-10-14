#include "common/hostinfo.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <fstream>

#include "build_info.hpp"
#include "common/sysfs.hpp"

namespace hwdiag {

namespace {

std::string compiler_name() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

}  // namespace

std::string read_cpu_model(const std::string& root) {
  std::ifstream in(sysfs::under(root, "/proc/cpuinfo"));
  std::string line;
  std::string fallback;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = sysfs::trim(line.substr(0, colon));
    const std::string value = sysfs::trim(line.substr(colon + 1));
    if (key == "model name" && !value.empty()) return value;
    if ((key == "Model" || key == "Hardware" || key == "cpu model") && fallback.empty()) fallback = value;
  }
  return fallback.empty() ? "unknown" : fallback;
}

std::string read_turbo(const std::string& root) {
  if (auto v = sysfs::read_int(sysfs::under(root, "/sys/devices/system/cpu/intel_pstate/no_turbo"))) {
    return *v == 0 ? "on" : "off";
  }
  if (auto v = sysfs::read_int(sysfs::under(root, "/sys/devices/system/cpu/cpufreq/boost"))) {
    return *v != 0 ? "on" : "off";
  }
  return "unknown";
}

std::string read_smt(const std::string& root) {
  if (auto v = sysfs::read_int(sysfs::under(root, "/sys/devices/system/cpu/smt/active"))) {
    return *v != 0 ? "on" : "off";
  }
  return "unknown";
}

HostInfo collect_host_info(const std::string& root) {
  HostInfo h;
  char name[256] = {};
  if (gethostname(name, sizeof name - 1) == 0) h.hostname = name;
  utsname u{};
  if (uname(&u) == 0) h.kernel = u.release;
  h.cpu_model = read_cpu_model(root);

  int processors = 0;
  std::ifstream in(sysfs::under(root, "/proc/cpuinfo"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("processor", 0) == 0) ++processors;
  }
  h.logical_cpus = processors > 0 ? processors : static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

  if (auto gov = sysfs::read_text(
          sysfs::under(root, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"))) {
    if (!gov->empty()) h.governor = *gov;
  }
  h.turbo = read_turbo(root);
  h.smt = read_smt(root);
  h.compiler = compiler_name();
  h.compiler_flags = build::kCompilerFlags;
  h.build_type = build::kBuildType;
  return h;
}

json::Value to_json(const HostInfo& h) {
  auto v = json::Value::object();
  v["hostname"] = h.hostname;
  v["kernel"] = h.kernel;
  v["cpu_model"] = h.cpu_model;
  v["logical_cpus"] = h.logical_cpus;
  v["governor"] = h.governor ? json::Value(*h.governor) : json::Value(nullptr);
  v["turbo"] = h.turbo;
  v["smt"] = h.smt;
  v["compiler"] = h.compiler;
  v["compiler_flags"] = h.compiler_flags;
  v["build_type"] = h.build_type;
  return v;
}

}  // namespace hwdiag
