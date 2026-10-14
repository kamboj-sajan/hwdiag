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

// Hooks around each timed repetition, used to read hardware counters.
// begin() runs just before the timer starts; end(steps) just after it stops
// and returns that repetition's counter readings (or null).
struct RepHooks {
  std::function<void()> begin;
  std::function<json::Value(uint64_t steps)> end;
  // Combines the per-repetition readings into the point's counters.
  std::function<json::Value(const std::vector<json::Value>&, uint64_t total_steps)> combine;
};

// progress (may be empty) is called after each size with a one-line summary.
Curve measure_curve(const CurveSpec& spec, const MeasureOptions& opts, const std::vector<SysfsCache>& caches,
                    const RepHooks* hooks = nullptr,
                    const std::function<void(const std::string&)>& progress = {});

json::Value to_json(const Curve& c);

}  // namespace hwdiag::memlat
