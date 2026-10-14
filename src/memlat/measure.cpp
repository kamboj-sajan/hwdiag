#include "memlat/measure.hpp"

#include <algorithm>
#include <cstdio>

#include "common/sysfs.hpp"

namespace hwdiag::memlat {

namespace {

// Written after every measurement so the compiler must keep the chase.
volatile uintptr_t g_sink;

}  // namespace

std::string curve_name(const CurveSpec& c) {
  return std::string(pattern_name(c.pattern)) + "/" + pages_name(c.pages);
}

Curve measure_curve(const CurveSpec& spec, const MeasureOptions& opts, const std::vector<SysfsCache>& caches,
                    perfctr::CounterGroup* counters, const std::function<void(const std::string&)>& progress) {
  const bool count = counters && counters->available();
  Curve curve;
  curve.spec = spec;
  std::vector<uint64_t> sizes;
  std::vector<double> medians;

  for (const uint64_t size : opts.sizes) {
    const size_t lines = size / kLineSize;
    if (lines < 2) continue;
    MappedBuffer buf(lines * kLineSize, spec.pages);

    // Linking the chain writes every line, so every page is faulted in here,
    // before any timing.
    void* p = build_chain(buf.data(), lines, spec.pattern, opts.seed + size);

    Point pt;
    pt.size_bytes = lines * kLineSize;
    if (spec.pages == Pages::Huge) pt.anon_huge_kib = anon_huge_kib(buf.data());

    // Warm-up: at least one full trip round the cycle, which also estimates
    // the latency so each repetition can be sized to about target_ms.
    const uint64_t warm_steps = std::max<uint64_t>(lines, 1u << 16);
    uint64_t t0 = now_ns();
    p = chase(p, warm_steps);
    uint64_t t1 = now_ns();
    pt.warmup_ns_per_load = static_cast<double>(t1 - t0) / static_cast<double>(warm_steps);
    const double est = std::max(pt.warmup_ns_per_load, 0.05);
    uint64_t steps = static_cast<uint64_t>(opts.target_ms * 1e6 / est);
    steps = std::clamp<uint64_t>(steps, 1u << 14, 4'000'000'000ull);
    steps = (steps + 15) / 16 * 16;
    pt.steps_per_rep = steps;

    std::vector<perfctr::Reading> readings;
    for (int rep = 0; rep < opts.reps; ++rep) {
      if (count) counters->start();
      t0 = now_ns();
      p = chase(p, steps);  // p carries on round the cycle across repetitions
      t1 = now_ns();
      if (count) readings.push_back(counters->stop());
      pt.samples_ns.push_back(static_cast<double>(t1 - t0) / static_cast<double>(steps));
    }
    g_sink = reinterpret_cast<uintptr_t>(p);

    pt.summary = summarize(pt.samples_ns);
    if (count) pt.counters = point_counters(perfctr::sum(readings), steps * static_cast<uint64_t>(opts.reps));
    if (progress) {
      char line[160];
      std::snprintf(line, sizeof line, "%-15s %6s  %8.2f ns  (spread %.1f%%)", curve_name(spec).c_str(),
                    sysfs::format_size(pt.size_bytes).c_str(), pt.summary.median, pt.summary.spread_pct);
      progress(line);
    }
    sizes.push_back(pt.size_bytes);
    medians.push_back(pt.summary.median);
    curve.points.push_back(std::move(pt));
  }

  curve.boundaries = detect_boundaries(sizes, medians, opts.threshold);
  curve.cache_matches = match_caches(curve.boundaries, caches);
  return curve;
}

json::Value point_counters(const perfctr::Reading& total, uint64_t loads) {
  if (!total.available || loads == 0) return nullptr;
  auto j = perfctr::to_json(total);
  auto per_load = json::Value::object();
  auto rate = [&](perfctr::Event e) {
    auto v = total.value(e);
    return v ? json::Value(*v / static_cast<double>(loads)) : json::Value(nullptr);
  };
  per_load["cycles"] = rate(perfctr::Event::Cycles);
  per_load["instructions"] = rate(perfctr::Event::Instructions);
  per_load["l1d_read_misses"] = rate(perfctr::Event::L1dReadMisses);
  per_load["dtlb_read_misses"] = rate(perfctr::Event::DtlbReadMisses);
  per_load["cache_misses"] = rate(perfctr::Event::CacheMisses);
  j["per_load"] = std::move(per_load);
  return j;
}

json::Value to_json(const Curve& c) {
  auto v = json::Value::object();
  v["name"] = curve_name(c.spec);
  v["pattern"] = pattern_name(c.spec.pattern);
  v["pages"] = pages_name(c.spec.pages);
  auto points = json::Value::array();
  for (const auto& p : c.points) {
    auto j = json::Value::object();
    j["size_bytes"] = p.size_bytes;
    j["size_label"] = sysfs::format_size(p.size_bytes);
    j["steps_per_rep"] = p.steps_per_rep;
    j["warmup_ns"] = p.warmup_ns_per_load;
    j["median_ns"] = p.summary.median;
    j["min_ns"] = p.summary.min;
    j["max_ns"] = p.summary.max;
    j["stdev_ns"] = p.summary.stdev;
    j["spread_pct"] = p.summary.spread_pct;
    auto samples = json::Value::array();
    for (double s : p.samples_ns) samples.push_back(s);
    j["samples_ns"] = std::move(samples);
    j["anon_huge_kib"] = p.anon_huge_kib ? json::Value(*p.anon_huge_kib) : json::Value(nullptr);
    j["counters"] = p.counters;
    points.push_back(std::move(j));
  }
  v["points"] = std::move(points);
  auto bounds = json::Value::array();
  for (const auto& b : c.boundaries) {
    auto j = json::Value::object();
    j["size_before"] = b.size_before;
    j["size_after"] = b.size_after;
    j["ns_before"] = b.ns_before;
    j["ns_after"] = b.ns_after;
    j["ratio"] = b.ratio;
    bounds.push_back(std::move(j));
  }
  v["boundaries"] = std::move(bounds);
  auto matches = json::Value::array();
  for (const auto& m : c.cache_matches) {
    auto j = json::Value::object();
    j["detected_bytes"] = m.detected_bytes;
    const bool matched = m.level != 0;
    j["level"] = matched ? json::Value(m.level) : json::Value(nullptr);
    j["sysfs_bytes"] = matched ? json::Value(m.sysfs_bytes) : json::Value(nullptr);
    j["ratio"] = matched ? json::Value(m.ratio) : json::Value(nullptr);
    matches.push_back(std::move(j));
  }
  v["cache_comparison"] = std::move(matches);
  return v;
}

}  // namespace hwdiag::memlat
