// memlat: memory latency at each cache level, by pointer chasing.
#include <exception>
#include <iostream>
#include <sstream>

#include "common/affinity.hpp"
#include "common/args.hpp"
#include "common/memory.hpp"
#include "common/result.hpp"
#include "common/sysfs.hpp"
#include "inventory/inventory.hpp"
#include "memlat/measure.hpp"

using namespace hwdiag;
using namespace hwdiag::memlat;

namespace {

std::vector<uint64_t> parse_size_list(const std::string& text) {
  std::vector<uint64_t> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    auto s = sysfs::parse_size(item);
    if (!s) throw std::invalid_argument("--sizes: not a size: '" + item + "'");
    out.push_back(*s);
  }
  return out;
}

Pattern parse_pattern(const std::string& s) {
  if (s == "random") return Pattern::Random;
  if (s == "sequential") return Pattern::Sequential;
  throw std::invalid_argument("--pattern must be random or sequential");
}

Pages parse_pages(const std::string& s) {
  if (s == "4k") return Pages::Small;
  if (s == "huge") return Pages::Huge;
  throw std::invalid_argument("--pages must be 4k or huge");
}

}  // namespace

int main(int argc, char** argv) {
  ArgParser args("memlat",
                 "Measures load-to-use latency for buffer sizes from L1 to DRAM by chasing a\n"
                 "random cycle of cache lines (Sattolo's algorithm) on one pinned core.");
  args.option("min", "smallest buffer size", "4K");
  args.option("max", "largest buffer size", "1G");
  args.option("sizes", "comma-separated sizes instead of --min/--max (e.g. 32K,1M,256M)");
  args.option("reps", "timed repetitions per size (at least 5)", "7");
  args.option("target-ms", "approximate duration of one repetition", "100");
  args.option("cpu", "CPU to pin to (default: first allowed CPU)");
  args.option("pattern", "random or sequential", "random");
  args.option("pages", "4k or huge (transparent huge pages via madvise)", "4k");
  args.option("modes", "single (use --pattern/--pages) or all (random/4k, random/huge, sequential/4k)", "single");
  args.option("seed", "seed for the random chain", "42");
  args.option("threshold", "latency ratio between neighbouring sizes that marks a boundary", "1.5");
  args.option("mem-node", "bind memory to this NUMA node (set_mempolicy MPOL_BIND)");
  args.option("root", "filesystem root for reading sysfs cache sizes", "/");
  args.option("out", "write JSON here instead of stdout", "-");
  args.flag("quiet", "no progress lines on stderr");
  args.flag("compact", "write compact JSON");
  if (!args.parse(argc, argv)) {
    std::cerr << "memlat: " << args.error() << "\n\n" << args.usage();
    return kExitError;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return kExitOk;
  }

  try {
    std::vector<std::string> warnings;
    MeasureOptions opts;
    opts.reps = static_cast<int>(args.integer("reps"));
    if (opts.reps < 5) throw std::invalid_argument("--reps must be at least 5");
    opts.target_ms = args.number("target-ms");
    if (opts.target_ms <= 0) throw std::invalid_argument("--target-ms must be positive");
    opts.seed = static_cast<uint64_t>(args.integer("seed"));
    opts.threshold = args.number("threshold");
    opts.sizes = args.given("sizes") ? parse_size_list(args.str("sizes"))
                                     : default_sizes(args.size("min"), args.size("max"));
    if (opts.sizes.empty()) throw std::invalid_argument("no buffer sizes to measure");

    // Keep every buffer (plus its index array) under half of available memory.
    const std::string root = args.str("root");
    const uint64_t avail = mem_available_bytes();
    if (avail > 0) {
      std::vector<uint64_t> kept;
      for (uint64_t s : opts.sizes) {
        if (s + s / 16 <= avail / 2) {
          kept.push_back(s);
        } else {
          warnings.push_back("skipped " + sysfs::format_size(s) + ": more than half of MemAvailable");
        }
      }
      opts.sizes = kept;
    }

    const auto cpus = allowed_cpus();
    const int cpu = args.given("cpu") ? static_cast<int>(args.integer("cpu")) : cpus.front();
    std::string err;
    bool pinned = pin_thread(cpu, &err);
    if (!pinned) warnings.push_back("not pinned: " + err);

    json::Value mem_node = nullptr;
    if (args.given("mem-node")) {
      const int node = static_cast<int>(args.integer("mem-node"));
      if (!bind_memory_to_node(node, &err)) throw std::runtime_error(err);
      mem_node = node;
    }

    std::vector<SysfsCache> caches;
    auto sys_caches = json::Value::array();
    for (const auto& c : inventory::parse_caches(root, cpu)) {
      caches.push_back({c.level, c.type, c.size_bytes});
      auto j = json::Value::object();
      j["level"] = c.level;
      j["type"] = c.type;
      j["size_bytes"] = c.size_bytes;
      sys_caches.push_back(std::move(j));
    }

    std::vector<CurveSpec> specs;
    const std::string modes = args.str("modes");
    if (modes == "all") {
      specs = {{Pattern::Random, Pages::Small}, {Pattern::Random, Pages::Huge}, {Pattern::Sequential, Pages::Small}};
    } else if (modes == "single") {
      specs = {{parse_pattern(args.str("pattern")), parse_pages(args.str("pages"))}};
    } else {
      throw std::invalid_argument("--modes must be single or all");
    }

    const bool quiet = args.on("quiet");
    auto progress = [quiet](const std::string& line) {
      if (!quiet) std::cerr << "memlat: " << line << "\n";
    };

    auto curves = json::Value::array();
    std::vector<Curve> measured;
    for (const auto& spec : specs) {
      measured.push_back(measure_curve(spec, opts, caches, nullptr, progress));
      curves.push_back(to_json(measured.back()));
      if (spec.pages == Pages::Huge) {
        bool any_huge = false;
        for (const auto& p : measured.back().points) any_huge |= p.anon_huge_kib.value_or(0) > 0;
        if (!any_huge) warnings.push_back("huge pages requested but none backed the buffers (THP disabled?)");
      }
    }

    // DRAM latency: the largest size on the random/4k curve.
    json::Value dram = nullptr;
    const Curve* detect_from = nullptr;
    for (const auto& c : measured) {
      if (c.spec.pattern == Pattern::Random && c.spec.pages == Pages::Small && !c.points.empty()) {
        dram = c.points.back().summary.median;
        if (!detect_from) detect_from = &c;
      }
    }
    // The huge-page curve is the better cache-size detector: no TLB steps.
    for (const auto& c : measured) {
      if (c.spec.pattern == Pattern::Random && c.spec.pages == Pages::Huge) detect_from = &c;
    }

    auto params = json::Value::object();
    auto sizes = json::Value::array();
    for (uint64_t s : opts.sizes) sizes.push_back(s);
    params["sizes"] = std::move(sizes);
    params["reps"] = opts.reps;
    params["target_ms"] = opts.target_ms;
    params["cpu"] = cpu;
    params["pinned"] = pinned;
    params["modes"] = modes;
    params["seed"] = opts.seed;
    params["threshold"] = opts.threshold;
    params["mem_node"] = mem_node;
    params["root"] = root;

    auto results = json::Value::object();
    results["sysfs_caches"] = std::move(sys_caches);
    results["curves"] = std::move(curves);
    results["dram_latency_ns"] = dram;
    results["dram_size_bytes"] = opts.sizes.empty() ? json::Value(nullptr) : json::Value(opts.sizes.back());
    if (detect_from) {
      results["detected_from"] = curve_name(detect_from->spec);
      results["detected_caches"] = to_json(*detect_from)["cache_comparison"];
    } else {
      results["detected_from"] = nullptr;
      results["detected_caches"] = json::Value::array();
    }

    if (!write_result(make_result("memlat", params, results, warnings), args.str("out"), !args.on("compact"))) {
      return kExitError;
    }
    return kExitOk;
  } catch (const std::exception& e) {
    std::cerr << "memlat: " << e.what() << "\n";
    return kExitError;
  }
}
