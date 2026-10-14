// membw: STREAM-style memory bandwidth from 1 to N pinned threads.
#include <cstdio>
#include <exception>
#include <iostream>
#include <sstream>

#include "common/affinity.hpp"
#include "common/args.hpp"
#include "common/memory.hpp"
#include "common/result.hpp"
#include "common/sysfs.hpp"
#include "inventory/inventory.hpp"
#include "membw/stream.hpp"

using namespace hwdiag;
using namespace hwdiag::membw;

namespace {

std::vector<int> parse_int_list(const std::string& text) {
  std::vector<int> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) out.push_back(std::stoi(item));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  ArgParser args("membw",
                 "Measures memory bandwidth with the STREAM copy, scale, add and triad kernels on\n"
                 "arrays at least 4x the last-level cache, from 1 to N threads pinned to cores.");
  args.option("array-size", "bytes per array (default: max(4 x LLC, 64M))");
  args.option("llc-multiple", "minimum array size as a multiple of the LLC", "4");
  args.option("min-array", "minimum bytes per array", "64M");
  args.option("threads", "comma-separated thread counts (default: 1..N, or powers of 2 above 16)");
  args.option("reps", "timed repetitions of the four kernels per thread count", "10");
  args.option("duration", "stress mode: run triad on all threads for this many seconds");
  args.option("interval", "stress mode: seconds between GB/s lines on stdout", "1");
  args.option("root", "filesystem root for reading sysfs", "/");
  args.option("out", "write JSON here instead of stdout", "-");
  args.flag("no-counters", "don't open hardware counters");
  args.flag("quiet", "no progress lines on stderr");
  args.flag("compact", "write compact JSON");
  if (!args.parse(argc, argv)) {
    std::cerr << "membw: " << args.error() << "\n\n" << args.usage();
    return kExitError;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return kExitOk;
  }

  try {
    std::vector<std::string> warnings;
    const std::string root = args.str("root");
    const bool quiet = args.on("quiet");

    // Array size: at least llc-multiple x the last-level cache, so the
    // arrays can't sit in cache, and at least min-array.
    const auto caches = inventory::parse_caches(root);
    const auto llc = inventory::last_level_cache(caches);
    uint64_t llc_bytes = llc ? llc->size_bytes : 0;
    if (!llc) warnings.push_back("no last-level cache size in sysfs; using --min-array");
    uint64_t array_bytes = 0;
    if (args.given("array-size")) {
      array_bytes = args.size("array-size");
    } else {
      array_bytes = std::max<uint64_t>(static_cast<uint64_t>(args.number("llc-multiple") * static_cast<double>(llc_bytes)),
                                       args.size("min-array"));
    }
    const uint64_t avail = mem_available_bytes(root);
    if (avail > 0 && 3 * array_bytes > avail / 2) {
      const uint64_t shrunk = avail / 2 / 3;
      warnings.push_back("arrays shrunk from " + sysfs::format_size(array_bytes) + " to " +
                         sysfs::format_size(shrunk) + " each to stay under half of MemAvailable");
      array_bytes = shrunk;
    }
    if (llc_bytes && array_bytes < 4 * llc_bytes) {
      warnings.push_back("arrays are smaller than 4x the LLC: results may include cache bandwidth");
    }
    const size_t elements = static_cast<size_t>(array_bytes / sizeof(double));
    if (elements < 1024) throw std::invalid_argument("array size too small");

    // Workers go on separate physical cores before any SMT sibling is used.
    const auto cpus = spread_order(read_cpu_topology(allowed_cpus(), root));
    const int n = static_cast<int>(cpus.size());

    auto params = json::Value::object();
    params["array_bytes"] = static_cast<uint64_t>(elements * sizeof(double));
    params["elements"] = static_cast<uint64_t>(elements);
    params["llc_bytes"] = llc_bytes;
    params["llc_multiple"] = args.number("llc-multiple");
    params["root"] = root;
    auto cpu_order = json::Value::array();
    for (int c : cpus) cpu_order.push_back(c);
    params["cpu_order"] = std::move(cpu_order);

    if (args.given("duration")) {
      StressOptions so;
      so.elements = elements;
      so.cpus = cpus;
      so.duration_s = args.number("duration");
      so.interval_s = args.number("interval");
      if (so.duration_s <= 0 || so.interval_s <= 0) throw std::invalid_argument("--duration and --interval must be positive");
      auto samples = json::Value::array();
      so.sample = [&samples](double t, double gbs) {
        // One line per interval for the harness's stress command.
        std::printf("{\"t\": %.3f, \"gbs\": %.4f}\n", t, gbs);
        std::fflush(stdout);
        auto s = json::Value::object();
        s["t"] = t;
        s["gbs"] = gbs;
        samples.push_back(std::move(s));
      };
      const double mean = run_stress(so);
      params["mode"] = "stress";
      params["duration_s"] = so.duration_s;
      params["interval_s"] = so.interval_s;
      params["threads"] = n;
      auto results = json::Value::object();
      results["mode"] = "stress";
      results["threads"] = n;
      results["samples"] = std::move(samples);
      results["mean_gbs"] = mean;
      std::string out = args.str("out");
      if (out == "-") out = "";  // stdout already carries the samples; only write a file if asked
      if (!out.empty() && !write_result(make_result("membw", params, results, warnings, root), out)) return kExitError;
      return kExitOk;
    }

    SweepOptions opts;
    opts.elements = elements;
    opts.cpus = cpus;
    opts.thread_counts = args.given("threads") ? parse_int_list(args.str("threads")) : thread_counts(n);
    opts.reps = static_cast<int>(args.integer("reps"));
    if (opts.reps < 1 || opts.reps > 200) throw std::invalid_argument("--reps must be 1..200");
    opts.counters = !args.on("no-counters");
    for (int t : opts.thread_counts) {
      if (t < 1 || t > n) throw std::invalid_argument("thread count " + std::to_string(t) + " outside 1.." + std::to_string(n));
    }
    if (!quiet) {
      std::cerr << "membw: 3 arrays x " << sysfs::format_size(elements * sizeof(double)) << ", " << opts.reps
                << " reps, threads up to " << opts.thread_counts.back() << "\n";
      opts.progress = [](const std::string& line) { std::cerr << "membw: " << line << "\n"; };
    }
    const SweepResult sweep = run_sweep(opts);
    if (!sweep.counters_reason.empty()) warnings.push_back("hardware counters unavailable: " + sweep.counters_reason);

    params["mode"] = "sweep";
    params["reps"] = opts.reps;
    auto tc = json::Value::array();
    for (int t : opts.thread_counts) tc.push_back(t);
    params["thread_counts"] = std::move(tc);

    auto results = json::Value::object();
    results["mode"] = "sweep";
    auto runs = json::Value::array();
    double peak = 0.0;
    std::string peak_kernel;
    int peak_threads = 0;
    bool all_valid = true;
    for (const auto& r : sweep.runs) {
      runs.push_back(to_json(r));
      all_valid &= r.validated;
      for (size_t ki = 0; ki < kKernels.size(); ++ki) {
        if (r.kernels[ki].summary.max > peak) {
          peak = r.kernels[ki].summary.max;
          peak_kernel = kernel_name(kKernels[ki]);
          peak_threads = r.threads;
        }
      }
    }
    results["runs"] = std::move(runs);
    auto pk = json::Value::object();
    pk["gbs"] = peak;
    pk["kernel"] = peak_kernel;
    pk["threads"] = peak_threads;
    results["peak"] = std::move(pk);
    results["validated"] = all_valid;
    auto ctr = json::Value::object();
    ctr["available"] = sweep.counters_reason.empty() && opts.counters;
    ctr["reason"] = sweep.counters_reason.empty() ? (opts.counters ? json::Value(nullptr) : json::Value("disabled with --no-counters"))
                                                  : json::Value(sweep.counters_reason);
    results["counters"] = std::move(ctr);
    if (!all_valid) warnings.push_back("validation failed: array values differ from the expected STREAM values");

    if (!write_result(make_result("membw", params, results, warnings, root), args.str("out"), !args.on("compact"))) {
      return kExitError;
    }
    return all_valid ? kExitOk : kExitProblem;
  } catch (const std::exception& e) {
    std::cerr << "membw: " << e.what() << "\n";
    return kExitError;
  }
}
