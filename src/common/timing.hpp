#pragma once

#include <cstdint>
#include <vector>

#include "common/json.hpp"

namespace hwdiag {

// Nanoseconds from CLOCK_MONOTONIC_RAW: not slewed by NTP, so short
// intervals are measured against the raw hardware clock.
uint64_t now_ns();

struct Summary {
  size_t n = 0;
  double median = 0.0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stdev = 0.0;       // sample standard deviation (n - 1)
  double spread_pct = 0.0;  // (max - min) / median * 100
};

// Summary of a set of repeated measurements. Empty input gives n = 0 and
// NaN for every statistic.
Summary summarize(std::vector<double> samples);

double median(std::vector<double> samples);

json::Value to_json(const Summary& s);

}  // namespace hwdiag
