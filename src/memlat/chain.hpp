// Pointer-chasing chains for memory latency measurement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hwdiag::memlat {

inline constexpr size_t kLineSize = 64;

enum class Pattern { Random, Sequential };
const char* pattern_name(Pattern p);

// Sattolo's algorithm: a uniformly random cyclic permutation. perm[i] is the
// successor of i, and following it from any start visits all n indices
// before returning. (Fisher-Yates draws j from [0, i]; Sattolo draws from
// [0, i-1], which rules out every permutation with more than one cycle.)
std::vector<uint32_t> sattolo(size_t n, uint64_t seed);

// Length of the cycle through start (n means a single cycle).
size_t cycle_length(const std::vector<uint32_t>& perm, size_t start = 0);

// Links the 64-byte lines of buf (lines * 64 bytes) into one cycle: each
// line's first 8 bytes point at the next line. Random uses sattolo();
// sequential links line i to i+1. Writing the links touches every page.
// Returns the start of the chain (line 0).
void* build_chain(void* buf, size_t lines, Pattern pattern, uint64_t seed);

// Follows the chain for steps loads (p = *p) and returns where it ended.
// Each load depends on the previous one, so the time per step is the load
// latency. Not inlined, so the compiler can't fold it into the caller.
void* chase(void* start, uint64_t steps);

// 2^k and 1.5 * 2^k from min to max inclusive (two sizes per doubling).
std::vector<uint64_t> default_sizes(uint64_t min_bytes, uint64_t max_bytes);

// A place where latency rose by more than the threshold. Consecutive jumps
// are merged, so one transition that spans several sizes is one boundary.
struct Boundary {
  size_t first_index = 0;      // index of the first size after the jump
  uint64_t size_before = 0;    // last size before the jump: the detected cache size
  uint64_t size_after = 0;
  double ns_before = 0.0;
  double ns_after = 0.0;
  double ratio = 0.0;          // ns_after / ns_before
};

std::vector<Boundary> detect_boundaries(const std::vector<uint64_t>& sizes, const std::vector<double>& ns,
                                        double threshold = 1.5);

struct SysfsCache {
  int level = 0;
  std::string type;
  uint64_t size_bytes = 0;
};

struct CacheMatch {
  uint64_t detected_bytes = 0;
  int level = 0;               // matched sysfs Data/Unified cache; 0 = unmatched
  uint64_t sysfs_bytes = 0;
  double ratio = 0.0;          // detected / sysfs
};

// Pairs boundaries with Data/Unified caches in size order (the k-th step
// with the k-th level where possible), as many pairs as possible, closest in
// log2 size. Extra boundaries (TLB reach, noise) stay unmatched.
std::vector<CacheMatch> match_caches(const std::vector<Boundary>& boundaries,
                                     const std::vector<SysfsCache>& caches);

}  // namespace hwdiag::memlat
