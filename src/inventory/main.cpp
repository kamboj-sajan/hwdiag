// inventory: read the machine's hardware info and flag problems.
#include <exception>
#include <iostream>

#include "common/args.hpp"
#include "common/result.hpp"
#include "inventory/inventory.hpp"

using namespace hwdiag;

int main(int argc, char** argv) {
  ArgParser args("inventory", "Reads CPU, cache, memory, NUMA, PCIe, sensor and EDAC information from\n"
                              "/proc and /sys, runs health checks and writes one JSON result.");
  args.option("root", "filesystem root to read /proc and /sys from", "/");
  args.option("temp-threshold", "warn above this temperature in degrees C", "85");
  args.option("out", "write JSON here instead of stdout", "-");
  args.flag("compact", "write compact JSON");
  if (!args.parse(argc, argv)) {
    std::cerr << "inventory: " << args.error() << "\n\n" << args.usage();
    return kExitError;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return kExitOk;
  }
  try {
    const std::string root = args.str("root");
    const double threshold = args.number("temp-threshold");

    const auto inv = inventory::collect(root);
    const auto checks = inventory::run_checks(inv, threshold);
    const std::string status = inventory::overall_status(checks);

    auto params = json::Value::object();
    params["root"] = root;
    params["temp_threshold_c"] = threshold;

    auto results = inventory::to_json(inv);
    results["checks"] = inventory::to_json(checks);
    results["status"] = status;

    std::vector<std::string> warnings;
    for (const auto& c : checks) {
      if (c.level == inventory::Level::Warn || c.level == inventory::Level::Fail) {
        warnings.push_back(c.check + " " + c.subject + ": " + c.message);
      }
    }
    if (!write_result(make_result("inventory", params, results, warnings, root), args.str("out"),
                      !args.on("compact"))) {
      return kExitError;
    }
    return status == "fail" ? kExitProblem : kExitOk;
  } catch (const std::exception& e) {
    std::cerr << "inventory: " << e.what() << "\n";
    return kExitError;
  }
}
