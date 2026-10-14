// The memlat measurement loop: one latency curve over a list of sizes.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "common/json.hpp"
#include "common/memory.hpp"
#include "common/timing.hpp"
#include "memlat/chain.hpp"
#include "perfctr/perfctr.hpp"

namespace hwdiag::memlat {

struct CurveSpec {
  Pattern pattern = Pattern::Random;
  Pages pages = Pages::Small;
};

std::string curve_name(const CurveSpec& c);  // "random/4k"

struct MeasureOptions {
  std::vector<uint64_t> sizes;
  int reps = 7;
  double target_ms = 100.0;
  uint64_t seed = 42;
  double threshold = 1.5;
};

struct Point {
  uint64_t size_bytes = 0;
  uint64_t steps_per_rep = 0;
  double warmup_ns_per_load = 0.0;
  std::vector<double> samples_ns;
  Summary summary;
  std::optional<uint64_t> anon_huge_kib;
  json::Value counters;  // null when hardware counters are unavailable
};

struct Curve {
  CurveSpec spec;
  std::vector<Point> points;
  std::vector<Boundary> boundaries;
  std::vector<CacheMatch> cache_matches;
};

// counters (may be null) is started and stopped around every timed
// repetition; the readings are summed per size. progress (may be empty) is
// called after each size with a one-line summary.
Curve measure_curve(const CurveSpec& spec, const MeasureOptions& opts, const std::vector<SysfsCache>& caches,
                    perfctr::CounterGroup* counters = nullptr,
                    const std::function<void(const std::string&)>& progress = {});

// Per-size counter summary: the perfctr reading plus per-load rates
// (cycles, L1D and dTLB read misses per load). Null when unavailable.
json::Value point_counters(const perfctr::Reading& total, uint64_t loads);

json::Value to_json(const Curve& c);

}  // namespace hwdiag::memlat
