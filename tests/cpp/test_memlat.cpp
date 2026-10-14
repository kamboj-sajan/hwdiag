#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "common/memory.hpp"
#include "memlat/chain.hpp"
#include "memlat/measure.hpp"

using namespace hwdiag;
using namespace hwdiag::memlat;

TEST(Sattolo, AlwaysProducesASingleCycle) {
  for (size_t n : {1, 2, 3, 4, 5, 7, 8, 16, 63, 64, 100, 1000, 4096, 65537}) {
    for (uint64_t seed = 0; seed < 25; ++seed) {
      const auto perm = sattolo(n, seed);
      ASSERT_EQ(perm.size(), n);
      EXPECT_EQ(cycle_length(perm, 0), n) << "n=" << n << " seed=" << seed;
      if (n > 1) {
        for (size_t i = 0; i < n; ++i) EXPECT_NE(perm[i], i) << "fixed point would be its own cycle";
      }
    }
  }
}

TEST(Sattolo, IsAPermutation) {
  const auto perm = sattolo(10'000, 7);
  std::set<uint32_t> seen(perm.begin(), perm.end());
  EXPECT_EQ(seen.size(), perm.size());
  EXPECT_EQ(*seen.rbegin(), perm.size() - 1);
}

TEST(Sattolo, SeedChangesTheOrderAndIsReproducible) {
  EXPECT_EQ(sattolo(1000, 1), sattolo(1000, 1));
  EXPECT_NE(sattolo(1000, 1), sattolo(1000, 2));
}

TEST(Sattolo, SmallestCasesAreTheOnlyPossibleCycles) {
  EXPECT_EQ(sattolo(1, 3), (std::vector<uint32_t>{0}));
  EXPECT_EQ(sattolo(2, 3), (std::vector<uint32_t>{1, 0}));
}

TEST(Sattolo, FisherYatesWouldFailTheSameCheck) {
  // The check is not vacuous: a permutation with two cycles is detected.
  const std::vector<uint32_t> two_cycles = {1, 0, 3, 2};
  EXPECT_EQ(cycle_length(two_cycles, 0), 2u);
}

namespace {

// Walks the chain and returns how many distinct lines it visits before
// coming back to the start (0 if it doesn't come back within lines steps).
size_t walk(void* start, size_t lines, const char* base) {
  std::set<size_t> seen;
  void* p = start;
  for (size_t step = 0; step < lines; ++step) {
    const auto offset = static_cast<size_t>(static_cast<const char*>(p) - base);
    EXPECT_EQ(offset % kLineSize, 0u);
    seen.insert(offset / kLineSize);
    p = *static_cast<void**>(p);
  }
  return p == start ? seen.size() : 0;
}

}  // namespace

TEST(Chain, RandomChainVisitsEveryLineOnce) {
  for (size_t lines : {2, 3, 64, 1000, 16384}) {
    MappedBuffer buf(lines * kLineSize, Pages::Small);
    void* start = build_chain(buf.data(), lines, Pattern::Random, 99);
    EXPECT_EQ(walk(start, lines, static_cast<char*>(buf.data())), lines);
  }
}

TEST(Chain, SequentialChainVisitsLinesInOrder) {
  const size_t lines = 256;
  MappedBuffer buf(lines * kLineSize, Pages::Small);
  auto* base = static_cast<char*>(buf.data());
  void* start = build_chain(base, lines, Pattern::Sequential, 0);
  EXPECT_EQ(*static_cast<void**>(start), base + kLineSize);
  EXPECT_EQ(walk(start, lines, base), lines);
}

TEST(Chain, ChaseEndsWhereStepByStepWalkingEnds) {
  const size_t lines = 1000;
  MappedBuffer buf(lines * kLineSize, Pages::Small);
  void* start = build_chain(buf.data(), lines, Pattern::Random, 5);
  for (uint64_t steps : {0ull, 1ull, 15ull, 16ull, 17ull, 999ull, 1000ull, 12345ull}) {
    void* expect = start;
    for (uint64_t i = 0; i < steps; ++i) expect = *static_cast<void**>(expect);
    EXPECT_EQ(chase(start, steps), expect) << steps;
  }
  EXPECT_EQ(chase(start, lines), start);  // a full lap returns home
}

TEST(Sizes, TwoPerDoublingFromMinToMax) {
  EXPECT_EQ(default_sizes(4096, 65536),
            (std::vector<uint64_t>{4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536}));
  const auto full = default_sizes(4096, 1ull << 30);
  EXPECT_EQ(full.front(), 4096u);
  EXPECT_EQ(full.back(), 1ull << 30);
  EXPECT_EQ(full.size(), 37u);
  EXPECT_TRUE(default_sizes(0, 10).empty());
}

TEST(Boundaries, FindsEachCacheStep) {
  // L1 to 32K, L2 to 256K, L3 to 8M, then DRAM.
  const std::vector<uint64_t> sizes = {16 << 10, 32 << 10, 64 << 10, 128 << 10, 256 << 10, 512 << 10,
                                       4 << 20,  8 << 20,  16 << 20, 64 << 20};
  const std::vector<double> ns = {1.0, 1.0, 3.5, 3.6, 3.7, 11.0, 11.5, 12.0, 70.0, 80.0};
  const auto b = detect_boundaries(sizes, ns, 1.5);
  ASSERT_EQ(b.size(), 3u);
  EXPECT_EQ(b[0].size_before, 32u << 10);
  EXPECT_EQ(b[1].size_before, 256u << 10);
  EXPECT_EQ(b[2].size_before, 8u << 20);
  EXPECT_NEAR(b[2].ratio, 70.0 / 12.0, 1e-9);
}

TEST(Boundaries, MergesAGradualTransitionIntoOne) {
  const std::vector<uint64_t> sizes = {1, 2, 3, 4, 5, 6};
  const std::vector<double> ns = {10.0, 10.5, 16.0, 25.0, 40.0, 42.0};
  const auto b = detect_boundaries(sizes, ns, 1.5);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0].size_before, 2u);
  EXPECT_EQ(b[0].size_after, 5u);
  EXPECT_NEAR(b[0].ratio, 40.0 / 10.5, 1e-9);
}

TEST(Boundaries, IgnoresNoiseBelowTheThreshold) {
  const std::vector<uint64_t> sizes = {1, 2, 3, 4, 5};
  const std::vector<double> ns = {10.0, 12.0, 11.0, 14.0, 13.5};
  EXPECT_TRUE(detect_boundaries(sizes, ns, 1.5).empty());
  // A latency drop is not a boundary either.
  EXPECT_TRUE(detect_boundaries({1, 2}, {50.0, 10.0}, 1.5).empty());
}

TEST(Boundaries, MatchesStepsToCacheLevelsInOrder) {
  const std::vector<SysfsCache> caches = {
      {1, "Data", 48 << 10}, {1, "Instruction", 32 << 10}, {2, "Unified", 2 << 20}, {3, "Unified", 260ull << 20}};
  std::vector<Boundary> b(2);
  b[0].size_before = 32 << 10;   // nearer L1d (48K) than L1i is ignored
  b[1].size_before = 1536 << 10; // 1.5M -> L2 (2M)
  const auto m = match_caches(b, caches);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m[0].level, 1);
  EXPECT_EQ(m[0].sysfs_bytes, 48u << 10);
  EXPECT_NEAR(m[0].ratio, 32.0 / 48.0, 1e-9);
  EXPECT_EQ(m[1].level, 2);
  EXPECT_NEAR(m[1].ratio, 0.75, 1e-9);
}

TEST(Boundaries, SmallEffectiveL3StillMatchesL3) {
  // A VM sees a fraction of the host's L3: the third step is far below the
  // sysfs size but is still the L3 step, not a second L2 step.
  const std::vector<SysfsCache> caches = {{1, "Data", 48 << 10}, {2, "Unified", 2 << 20}, {3, "Unified", 260ull << 20}};
  std::vector<Boundary> b(3);
  b[0].size_before = 48 << 10;
  b[1].size_before = 1 << 20;
  b[2].size_before = 12 << 20;
  const auto m = match_caches(b, caches);
  EXPECT_EQ(m[0].level, 1);
  EXPECT_EQ(m[1].level, 2);
  EXPECT_EQ(m[2].level, 3);
}

TEST(Boundaries, ExtraStepsStayUnmatched) {
  // A TLB step at 256K between L1 and L2 boundaries.
  const std::vector<SysfsCache> caches = {{1, "Data", 32 << 10}, {2, "Unified", 1 << 20}};
  std::vector<Boundary> b(3);
  b[0].size_before = 32 << 10;
  b[1].size_before = 256 << 10;
  b[2].size_before = 1 << 20;
  const auto m = match_caches(b, caches);
  EXPECT_EQ(m[0].level, 1);
  EXPECT_EQ(m[1].level, 0);
  EXPECT_EQ(m[2].level, 2);
}

TEST(Measure, SmallCurveProducesPositiveMedians) {
  MeasureOptions opts;
  opts.sizes = {4096, 65536};
  opts.reps = 5;
  opts.target_ms = 1.0;
  const Curve c = measure_curve({Pattern::Random, Pages::Small}, opts, {});
  ASSERT_EQ(c.points.size(), 2u);
  for (const auto& p : c.points) {
    EXPECT_EQ(p.samples_ns.size(), 5u);
    EXPECT_GT(p.summary.median, 0.0);
    EXPECT_EQ(p.steps_per_rep % 16, 0u);
  }
  EXPECT_EQ(to_json(c).find("points")->size(), 2u);
}

TEST(Memory, BufferIsHugePageAligned) {
  MappedBuffer buf(100, Pages::Huge);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data()) % kHugePageSize, 0u);
  EXPECT_EQ(buf.mapped_size(), kHugePageSize);
  static_cast<char*>(buf.data())[0] = 1;
  EXPECT_TRUE(anon_huge_kib(buf.data()).has_value());
}
