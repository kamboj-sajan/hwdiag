#include "memlat/chain.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hwdiag::memlat {

namespace {

// splitmix64: small, fast and good enough to shuffle cache lines.
struct SplitMix64 {
  uint64_t state;
  uint64_t next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // Uniform in [0, bound) by multiply-shift; the bias is below 2^-32 for
  // any bound we use (at most 2^24 lines).
  uint64_t below(uint64_t bound) {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(next()) * bound) >> 64);
  }
};

}  // namespace

const char* pattern_name(Pattern p) { return p == Pattern::Random ? "random" : "sequential"; }

std::vector<uint32_t> sattolo(size_t n, uint64_t seed) {
  if (n > UINT32_MAX) throw std::invalid_argument("sattolo: too many elements");
  std::vector<uint32_t> perm(n);
  for (size_t i = 0; i < n; ++i) perm[i] = static_cast<uint32_t>(i);
  SplitMix64 rng{seed};
  for (size_t i = n; i-- > 1;) {
    const size_t j = rng.below(i);  // j in [0, i-1]: never i itself
    std::swap(perm[i], perm[j]);
  }
  return perm;
}

size_t cycle_length(const std::vector<uint32_t>& perm, size_t start) {
  if (perm.empty()) return 0;
  size_t len = 0;
  size_t i = start;
  do {
    i = perm[i];
    ++len;
  } while (i != start && len <= perm.size());
  return len;
}

void* build_chain(void* buf, size_t lines, Pattern pattern, uint64_t seed) {
  if (lines == 0) throw std::invalid_argument("build_chain: no lines");
  auto* base = static_cast<char*>(buf);
  auto line = [base](size_t i) { return reinterpret_cast<void**>(base + i * kLineSize); };
  if (pattern == Pattern::Sequential) {
    for (size_t i = 0; i < lines; ++i) *line(i) = line((i + 1) % lines);
  } else {
    const auto perm = sattolo(lines, seed);
    for (size_t i = 0; i < lines; ++i) *line(i) = line(perm[i]);
  }
  return base;
}

__attribute__((noinline)) void* chase(void* start, uint64_t steps) {
  void** p = static_cast<void**>(start);
#define HWDIAG_STEP p = static_cast<void**>(*p);
  for (uint64_t n = steps / 16; n; --n) {
    HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP
    HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP
    HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP
    HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP HWDIAG_STEP
  }
  for (uint64_t n = steps % 16; n; --n) {
    HWDIAG_STEP
  }
#undef HWDIAG_STEP
  return p;
}

std::vector<uint64_t> default_sizes(uint64_t min_bytes, uint64_t max_bytes) {
  std::vector<uint64_t> sizes;
  if (min_bytes == 0 || max_bytes < min_bytes) return sizes;
  uint64_t p = 1;
  while (p * 2 <= min_bytes) p *= 2;
  for (; p <= max_bytes; p *= 2) {
    if (p >= min_bytes) sizes.push_back(p);
    const uint64_t mid = p + p / 2;
    if (mid >= min_bytes && mid <= max_bytes) sizes.push_back(mid);
  }
  return sizes;
}

std::vector<Boundary> detect_boundaries(const std::vector<uint64_t>& sizes, const std::vector<double>& ns,
                                        double threshold) {
  std::vector<Boundary> out;
  const size_t n = std::min(sizes.size(), ns.size());
  size_t i = 1;
  while (i < n) {
    if (!(ns[i - 1] > 0.0) || !(ns[i] / ns[i - 1] > threshold)) {
      ++i;
      continue;
    }
    // Extend over consecutive jumps.
    size_t last = i;
    while (last + 1 < n && ns[last] > 0.0 && ns[last + 1] / ns[last] > threshold) ++last;
    Boundary b;
    b.first_index = i;
    b.size_before = sizes[i - 1];
    b.size_after = sizes[last];
    b.ns_before = ns[i - 1];
    b.ns_after = ns[last];
    b.ratio = ns[last] / ns[i - 1];
    out.push_back(b);
    i = last + 1;
  }
  return out;
}

std::vector<CacheMatch> match_caches(const std::vector<Boundary>& boundaries,
                                     const std::vector<SysfsCache>& caches) {
  // Data/Unified caches, smallest first.
  std::vector<SysfsCache> levels;
  for (const auto& c : caches) {
    if (c.type != "Instruction" && c.size_bytes > 0) levels.push_back(c);
  }
  std::sort(levels.begin(), levels.end(),
            [](const SysfsCache& a, const SysfsCache& b) { return a.size_bytes < b.size_bytes; });

  // Order-preserving matching: boundary i and cache j can only pair if every
  // smaller boundary pairs with a smaller cache. Maximise the number of
  // pairs, then minimise the total log2 distance (dynamic programming).
  // Matching by nearest size alone would pair a VM's small effective L3
  // with L2 when sysfs reports the host's whole L3.
  const size_t m = boundaries.size(), k = levels.size();
  struct Cell {
    int pairs = 0;
    double cost = 0.0;
  };
  auto better = [](const Cell& a, const Cell& b) {
    return a.pairs != b.pairs ? a.pairs > b.pairs : a.cost < b.cost - 1e-12;
  };
  auto dist = [&](size_t i, size_t j) {
    return std::fabs(std::log2(static_cast<double>(boundaries[i].size_before)) -
                     std::log2(static_cast<double>(levels[j].size_bytes)));
  };
  std::vector<std::vector<Cell>> best(m + 1, std::vector<Cell>(k + 1));
  for (size_t i = 1; i <= m; ++i) {
    for (size_t j = 1; j <= k; ++j) {
      Cell c = best[i - 1][j];
      if (better(best[i][j - 1], c)) c = best[i][j - 1];
      Cell pair{best[i - 1][j - 1].pairs + 1, best[i - 1][j - 1].cost + dist(i - 1, j - 1)};
      if (better(pair, c)) c = pair;
      best[i][j] = c;
    }
  }
  std::vector<CacheMatch> out(m);
  for (size_t i = 0; i < m; ++i) out[i].detected_bytes = boundaries[i].size_before;
  for (size_t i = m, j = k; i > 0 && j > 0;) {
    const Cell pair{best[i - 1][j - 1].pairs + 1, best[i - 1][j - 1].cost + dist(i - 1, j - 1)};
    if (best[i][j].pairs == pair.pairs && std::fabs(best[i][j].cost - pair.cost) < 1e-9) {
      out[i - 1].level = levels[j - 1].level;
      out[i - 1].sysfs_bytes = levels[j - 1].size_bytes;
      out[i - 1].ratio = static_cast<double>(out[i - 1].detected_bytes) / static_cast<double>(out[i - 1].sysfs_bytes);
      --i;
      --j;
    } else if (best[i][j].pairs == best[i - 1][j].pairs && std::fabs(best[i][j].cost - best[i - 1][j].cost) < 1e-9) {
      --i;
    } else {
      --j;
    }
  }
  return out;
}

}  // namespace hwdiag::memlat
