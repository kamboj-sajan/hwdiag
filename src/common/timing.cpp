#include "common/timing.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>

namespace hwdiag {

uint64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

double median(std::vector<double> samples) {
  if (samples.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(samples.begin(), samples.end());
  const size_t n = samples.size();
  return n % 2 ? samples[n / 2] : 0.5 * (samples[n / 2 - 1] + samples[n / 2]);
}

Summary summarize(std::vector<double> samples) {
  Summary s;
  s.n = samples.size();
  if (samples.empty()) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    s.median = s.min = s.max = s.mean = s.stdev = s.spread_pct = nan;
    return s;
  }
  std::sort(samples.begin(), samples.end());
  s.min = samples.front();
  s.max = samples.back();
  s.median = median(samples);
  s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(s.n);
  if (s.n > 1) {
    double ss = 0.0;
    for (double x : samples) ss += (x - s.mean) * (x - s.mean);
    s.stdev = std::sqrt(ss / static_cast<double>(s.n - 1));
  }
  s.spread_pct = s.median != 0.0 ? (s.max - s.min) / s.median * 100.0
                                 : std::numeric_limits<double>::quiet_NaN();
  return s;
}

json::Value to_json(const Summary& s) {
  auto v = json::Value::object();
  v["n"] = s.n;
  v["median"] = s.median;
  v["min"] = s.min;
  v["max"] = s.max;
  v["mean"] = s.mean;
  v["stdev"] = s.stdev;
  v["spread_pct"] = s.spread_pct;
  return v;
}

}  // namespace hwdiag
