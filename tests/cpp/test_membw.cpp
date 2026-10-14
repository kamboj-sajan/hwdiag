#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "common/affinity.hpp"
#include "membw/stream.hpp"

using namespace hwdiag;
using namespace hwdiag::membw;

TEST(Membw, ThreadCountsAreOneToNThenPowersOfTwo) {
  EXPECT_EQ(thread_counts(4), (std::vector<int>{1, 2, 3, 4}));
  EXPECT_EQ(thread_counts(16).size(), 16u);
  EXPECT_EQ(thread_counts(32), (std::vector<int>{1, 2, 4, 8, 16, 32}));
  EXPECT_EQ(thread_counts(20), (std::vector<int>{1, 2, 4, 8, 16, 20}));
  EXPECT_TRUE(thread_counts(0).empty());
}

TEST(Membw, BytesCountedTheStreamWay) {
  EXPECT_EQ(bytes_per_element(Kernel::Copy), 16u);
  EXPECT_EQ(bytes_per_element(Kernel::Scale), 16u);
  EXPECT_EQ(bytes_per_element(Kernel::Add), 24u);
  EXPECT_EQ(bytes_per_element(Kernel::Triad), 24u);
}

TEST(Membw, KernelsComputeStreamFormulas) {
  std::vector<double> a = {1, 2, 3, 4}, b = {10, 20, 30, 40}, c = {0, 0, 0, 0};
  run_kernel(Kernel::Copy, a.data(), b.data(), c.data(), 0, 4);
  EXPECT_EQ(c, a);
  run_kernel(Kernel::Scale, a.data(), b.data(), c.data(), 0, 4);
  EXPECT_EQ(b, (std::vector<double>{3, 6, 9, 12}));
  run_kernel(Kernel::Add, a.data(), b.data(), c.data(), 1, 3);  // only a slice
  EXPECT_EQ(c, (std::vector<double>{1, 8, 12, 4}));
  run_kernel(Kernel::Triad, a.data(), b.data(), c.data(), 0, 4);
  EXPECT_EQ(a, (std::vector<double>{3 + 3 * 1, 6 + 3 * 8, 9 + 3 * 12, 12 + 3 * 4}));
}

TEST(Membw, ExpectedValuesFollowTheKernelSequence) {
  const Expected e0 = expected_after(0);
  EXPECT_EQ(e0.a, 1.0);
  EXPECT_EQ(e0.b, 2.0);
  EXPECT_EQ(e0.c, 0.0);
  const Expected e1 = expected_after(1);
  EXPECT_EQ(e1.c, 4.0);   // c = a + b, with b = 3a
  EXPECT_EQ(e1.b, 3.0);
  EXPECT_EQ(e1.a, 15.0);  // a = b + 3c
  EXPECT_EQ(expected_after(2).a, 225.0);
}

TEST(Membw, SpinBarrierKeepsThreadsInStep) {
  constexpr int kThreads = 4, kRounds = 2000;
  SpinBarrier barrier(kThreads);
  std::atomic<int> counter{0};
  std::atomic<bool> ok{true};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&] {
      for (int r = 0; r < kRounds; ++r) {
        counter.fetch_add(1);
        barrier.arrive_and_wait();
        if (counter.load() != (r + 1) * kThreads) ok = false;
        barrier.arrive_and_wait();
      }
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_TRUE(ok.load());
}

TEST(Membw, SmallSweepValidatesAndReportsBandwidth) {
  SweepOptions opts;
  opts.elements = 1 << 16;
  const auto cpus = allowed_cpus();
  opts.cpus = cpus;
  opts.thread_counts = {1};
  if (cpus.size() > 1) opts.thread_counts.push_back(2);
  opts.reps = 3;
  const SweepResult r = run_sweep(opts);
  ASSERT_EQ(r.runs.size(), opts.thread_counts.size());
  for (const auto& run : r.runs) {
    EXPECT_TRUE(run.validated) << run.max_rel_error;
    for (const auto& k : run.kernels) {
      EXPECT_EQ(k.samples_gbs.size(), 3u);
      EXPECT_GT(k.summary.max, 0.0);
    }
  }
  const auto j = to_json(r.runs[0]);
  EXPECT_NE(j.find("kernels")->find("triad"), nullptr);
}

TEST(Membw, StressModeReportsEachInterval) {
  StressOptions so;
  so.elements = 1 << 18;
  so.cpus = {allowed_cpus().front()};
  so.duration_s = 0.35;
  so.interval_s = 0.1;
  std::vector<double> times;
  so.sample = [&](double t, double gbs) {
    times.push_back(t);
    EXPECT_GE(gbs, 0.0);
  };
  const double mean = run_stress(so);
  EXPECT_GE(times.size(), 3u);
  EXPECT_GT(mean, 0.0);
  EXPECT_NEAR(times.back(), 0.35, 0.05);
}
