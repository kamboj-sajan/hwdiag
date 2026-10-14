// perfcheck: counts a fixed workload with perfctr so the counts can be
// cross-checked against `perf stat -e cycles:u,instructions:u perfcheck`.
//
// The counter group is started at the top of main and stopped at the end,
// so it sees almost everything perf stat sees (only the dynamic loader,
// static initialisers and the JSON output fall outside it).
#include <exception>
#include <iostream>

#include "common/args.hpp"
#include "common/memory.hpp"
#include "common/result.hpp"
#include "common/timing.hpp"
#include "memlat/chain.hpp"
#include "perfctr/perfctr.hpp"

using namespace hwdiag;

namespace {

volatile uint64_t g_sink;

// A deterministic arithmetic loop: a fixed number of instructions, no memory.
__attribute__((noinline)) uint64_t alu_work(uint64_t iterations) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < iterations; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  return x;
}

}  // namespace

int main(int argc, char** argv) {
  perfctr::CounterGroup counters;
  counters.start();
  const uint64_t t0 = now_ns();

  ArgParser args("perfcheck", "Counts a fixed ALU + pointer-chasing workload with perfctr, for\n"
                              "cross-checking against perf stat.");
  args.option("alu-iterations", "iterations of the xorshift loop", "200000000");
  args.option("chase-bytes", "buffer for the pointer chase", "32M");
  args.option("chase-steps", "dependent loads in the pointer chase", "4000000");
  args.option("out", "write JSON here instead of stdout", "-");
  if (!args.parse(argc, argv)) {
    std::cerr << "perfcheck: " << args.error() << "\n\n" << args.usage();
    return kExitError;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return kExitOk;
  }
  try {
    const uint64_t alu = static_cast<uint64_t>(args.integer("alu-iterations"));
    const uint64_t bytes = args.size("chase-bytes");
    const uint64_t steps = static_cast<uint64_t>(args.integer("chase-steps"));

    g_sink = alu_work(alu);
    MappedBuffer buf(bytes, Pages::Default);
    void* p = memlat::build_chain(buf.data(), bytes / memlat::kLineSize, memlat::Pattern::Random, 1);
    p = memlat::chase(p, steps);
    g_sink = reinterpret_cast<uintptr_t>(p);

    const uint64_t t1 = now_ns();
    const perfctr::Reading r = counters.stop();

    auto params = json::Value::object();
    params["alu_iterations"] = alu;
    params["chase_bytes"] = bytes;
    params["chase_steps"] = steps;
    auto results = json::Value::object();
    results["elapsed_s"] = static_cast<double>(t1 - t0) / 1e9;
    results["counters"] = perfctr::to_json(r);
    auto layout = json::Value::array();
    for (const auto& group : counters.layout()) {
      auto g = json::Value::array();
      for (auto e : group) g.push_back(perfctr::event_name(e));
      layout.push_back(std::move(g));
    }
    results["group_layout"] = std::move(layout);
    std::vector<std::string> warnings;
    if (!r.available) warnings.push_back("hardware counters unavailable: " + r.reason);
    if (!write_result(make_result("perfcheck", params, results, warnings), args.str("out"))) return kExitError;
    return kExitOk;
  } catch (const std::exception& e) {
    std::cerr << "perfcheck: " << e.what() << "\n";
    return kExitError;
  }
}
