// STREAM-style bandwidth kernels and the multi-threaded runner.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/json.hpp"
#include "common/timing.hpp"
#include "perfctr/perfctr.hpp"

namespace hwdiag::membw {

enum class Kernel { Copy, Scale, Add, Triad };
inline constexpr std::array<Kernel, 4> kKernels = {Kernel::Copy, Kernel::Scale, Kernel::Add, Kernel::Triad};
inline constexpr double kScalar = 3.0;

const char* kernel_name(Kernel k);

// Bytes STREAM counts per element: copy and scale read one array and write
// one (16 B); add and triad read two and write one (24 B). Write-allocate
// traffic (reading the destination line before writing it) is not counted.
size_t bytes_per_element(Kernel k);

// The kernels on elements [begin, end):
//   copy:  c = a        scale: b = s * c
//   add:   c = a + b    triad: a = b + s * c
void run_kernel(Kernel k, double* a, double* b, double* c, size_t begin, size_t end);

// STREAM's starting values and the values every element should hold after
// `passes` rounds of copy, scale, add, triad.
struct Expected {
  double a = 1.0;
  double b = 2.0;
  double c = 0.0;
};
Expected expected_after(int passes);

// Thread counts to measure: 1..n, or 1, 2, 4, ..., n when n > 16.
std::vector<int> thread_counts(int n);

// A sense-reversing spin barrier. Spinning (rather than sleeping) means all
// threads leave within nanoseconds of each other, so barrier-to-barrier
// timing covers the kernel and little else.
class SpinBarrier {
 public:
  explicit SpinBarrier(int participants) : n_(participants) {}
  void arrive_and_wait();

 private:
  const int n_;
  std::atomic<int> count_{0};
  std::atomic<uint64_t> generation_{0};
};

struct SweepOptions {
  size_t elements = 0;             // per array
  std::vector<int> cpus;           // CPU for thread i, in spread order
  std::vector<int> thread_counts;
  int reps = 10;
  bool counters = true;
  std::function<void(const std::string&)> progress;
};

struct KernelResult {
  std::vector<double> samples_gbs;
  Summary summary;  // over samples_gbs; "best" is summary.max
  perfctr::Reading counters;
};

struct ThreadResult {
  int threads = 0;
  std::vector<int> cpus;
  std::array<KernelResult, 4> kernels;
  bool validated = false;
  double max_rel_error = 0.0;
};

struct SweepResult {
  std::vector<ThreadResult> runs;
  std::string counters_reason;  // set when counters were unavailable
};

// For each thread count: fresh arrays first-touched by the workers that use
// them, one untimed warm-up pass, then reps timed passes of all four
// kernels, then validation of every element against expected_after().
SweepResult run_sweep(const SweepOptions& opts);

struct StressOptions {
  size_t elements = 0;
  std::vector<int> cpus;  // one worker per CPU
  double duration_s = 10.0;
  double interval_s = 1.0;
  // Called once per interval with seconds since start and GB/s in that interval.
  std::function<void(double t, double gbs)> sample;
};

// Runs triad on every CPU until duration_s has passed. Returns mean GB/s.
double run_stress(const StressOptions& opts);

json::Value to_json(const ThreadResult& r);

}  // namespace hwdiag::membw
