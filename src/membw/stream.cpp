#include "membw/stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>

#include "common/affinity.hpp"
#include "common/memory.hpp"

namespace hwdiag::membw {

namespace {

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

struct Arrays {
  MappedBuffer a, b, c;
  explicit Arrays(size_t elements)
      : a(elements * sizeof(double), Pages::Default),
        b(elements * sizeof(double), Pages::Default),
        c(elements * sizeof(double), Pages::Default) {}
  double* pa() const { return static_cast<double*>(a.data()); }
  double* pb() const { return static_cast<double*>(b.data()); }
  double* pc() const { return static_cast<double*>(c.data()); }
};

std::pair<size_t, size_t> slice(size_t n, int threads, int i) {
  const size_t begin = n * static_cast<size_t>(i) / static_cast<size_t>(threads);
  const size_t end = n * static_cast<size_t>(i + 1) / static_cast<size_t>(threads);
  return {begin, end};
}

}  // namespace

const char* kernel_name(Kernel k) {
  switch (k) {
    case Kernel::Copy: return "copy";
    case Kernel::Scale: return "scale";
    case Kernel::Add: return "add";
    case Kernel::Triad: return "triad";
  }
  return "unknown";
}

size_t bytes_per_element(Kernel k) {
  return (k == Kernel::Copy || k == Kernel::Scale) ? 2 * sizeof(double) : 3 * sizeof(double);
}

Expected expected_after(int passes) {
  Expected e;
  for (int i = 0; i < passes; ++i) {
    e.c = e.a;
    e.b = kScalar * e.c;
    e.c = e.a + e.b;
    e.a = e.b + kScalar * e.c;
  }
  return e;
}

std::vector<int> thread_counts(int n) {
  std::vector<int> out;
  if (n <= 0) return out;
  if (n <= 16) {
    for (int t = 1; t <= n; ++t) out.push_back(t);
    return out;
  }
  for (int t = 1; t < n; t *= 2) out.push_back(t);
  out.push_back(n);
  return out;
}

void SpinBarrier::arrive_and_wait() {
  const uint64_t gen = generation_.load(std::memory_order_acquire);
  if (count_.fetch_add(1, std::memory_order_acq_rel) + 1 == n_) {
    count_.store(0, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_release);
    return;
  }
  while (generation_.load(std::memory_order_acquire) == gen) cpu_relax();
}

SweepResult run_sweep(const SweepOptions& opts) {
  SweepResult out;
  const size_t n = opts.elements;
  for (const int threads : opts.thread_counts) {
    if (threads <= 0 || static_cast<size_t>(threads) > opts.cpus.size()) continue;
    Arrays arrays(n);
    SpinBarrier start(threads), done(threads);
    // per_thread[i][k]: counter readings of thread i for kernel k, all reps.
    std::vector<std::array<std::vector<perfctr::Reading>, 4>> per_thread(static_cast<size_t>(threads));
    std::vector<std::string> reasons(static_cast<size_t>(threads));
    ThreadResult tr;
    tr.threads = threads;
    tr.cpus.assign(opts.cpus.begin(), opts.cpus.begin() + threads);

    // The calling thread is worker 0 and also keeps time, so no extra
    // thread spins on a barrier and competes with the workers for a CPU.
    auto body = [&](int i) {
      pin_thread(opts.cpus[static_cast<size_t>(i)]);
      // Counters count the calling thread, so each worker opens its own.
      std::unique_ptr<perfctr::CounterGroup> ctr;
      if (opts.counters) {
        ctr = std::make_unique<perfctr::CounterGroup>();
        if (!ctr->available()) {
          reasons[static_cast<size_t>(i)] = ctr->reason();
          ctr.reset();
        }
      }
      const auto [begin, end] = slice(n, threads, i);
      double* a = arrays.pa();
      double* b = arrays.pb();
      double* c = arrays.pc();
      // First touch: this thread's pages land on its own NUMA node.
      for (size_t j = begin; j < end; ++j) {
        a[j] = 1.0;
        b[j] = 2.0;
        c[j] = 0.0;
      }
      start.arrive_and_wait();
      // Untimed warm-up pass, then the timed repetitions.
      for (Kernel k : kKernels) run_kernel(k, a, b, c, begin, end);
      done.arrive_and_wait();
      for (int rep = 0; rep < opts.reps; ++rep) {
        for (size_t ki = 0; ki < kKernels.size(); ++ki) {
          start.arrive_and_wait();
          const uint64_t t0 = now_ns();
          if (ctr) ctr->start();
          run_kernel(kKernels[ki], a, b, c, begin, end);
          if (ctr) per_thread[static_cast<size_t>(i)][ki].push_back(ctr->stop());
          done.arrive_and_wait();
          if (i == 0) {
            const uint64_t t1 = now_ns();
            const double bytes = static_cast<double>(bytes_per_element(kKernels[ki]) * n);
            tr.kernels[ki].samples_gbs.push_back(bytes / static_cast<double>(t1 - t0));  // bytes/ns == GB/s
          }
        }
      }
    };
    std::vector<std::thread> workers;
    for (int i = 1; i < threads; ++i) workers.emplace_back(body, i);
    body(0);
    for (auto& w : workers) w.join();

    for (size_t ki = 0; ki < kKernels.size(); ++ki) {
      tr.kernels[ki].summary = summarize(tr.kernels[ki].samples_gbs);
      std::vector<perfctr::Reading> all;
      for (const auto& t : per_thread) all.insert(all.end(), t[ki].begin(), t[ki].end());
      if (!all.empty() && all.size() == static_cast<size_t>(threads * opts.reps)) {
        tr.kernels[ki].counters = perfctr::sum(all);
      } else {
        tr.kernels[ki].counters.reason = reasons.empty() || reasons[0].empty() ? "counters off" : reasons[0];
      }
    }
    if (!reasons.empty() && !reasons[0].empty()) out.counters_reason = reasons[0];

    // Validate like STREAM: every element against the scalar recurrence.
    const Expected e = expected_after(opts.reps + 1);  // +1 for the warm-up
    double worst = 0.0;
    const double* a = arrays.pa();
    const double* b = arrays.pb();
    const double* c = arrays.pc();
    for (size_t j = 0; j < n; ++j) {
      worst = std::max({worst, std::fabs(a[j] - e.a) / std::fabs(e.a), std::fabs(b[j] - e.b) / std::fabs(e.b),
                        std::fabs(c[j] - e.c) / std::fabs(e.c)});
    }
    tr.max_rel_error = worst;
    tr.validated = worst <= 1e-13;

    if (opts.progress) {
      const auto& triad = tr.kernels[3].summary;
      char line[160];
      std::snprintf(line, sizeof line, "%2d thread(s): triad best %.2f GB/s, median %.2f GB/s%s", threads,
                    triad.max, triad.median, tr.validated ? "" : "  VALIDATION FAILED");
      opts.progress(line);
    }
    out.runs.push_back(std::move(tr));
  }
  return out;
}

double run_stress(const StressOptions& opts) {
  const size_t n = opts.elements;
  const int threads = static_cast<int>(opts.cpus.size());
  Arrays arrays(n);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<int> ready{0};
  constexpr size_t kChunk = 1u << 18;  // elements per progress update

  std::vector<std::thread> workers;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&, i] {
      pin_thread(opts.cpus[static_cast<size_t>(i)]);
      const auto [begin, end] = slice(n, threads, i);
      double* a = arrays.pa();
      double* b = arrays.pb();
      double* c = arrays.pc();
      for (size_t j = begin; j < end; ++j) {
        a[j] = 1.0;
        b[j] = 2.0;
        c[j] = 0.5;
      }
      ready.fetch_add(1, std::memory_order_release);
      while (!stop.load(std::memory_order_relaxed)) {
        for (size_t j = begin; j < end && !stop.load(std::memory_order_relaxed); j += kChunk) {
          const size_t e = std::min(end, j + kChunk);
          run_kernel(Kernel::Triad, a, b, c, j, e);
          bytes_done.fetch_add((e - j) * bytes_per_element(Kernel::Triad), std::memory_order_relaxed);
        }
      }
    });
  }

  // The clock starts once every worker has first-touched its slice, so the
  // samples cover the load itself, not page-fault time.
  while (ready.load(std::memory_order_acquire) < threads) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const uint64_t t_start = now_ns();
  uint64_t last_t = t_start;
  uint64_t last_bytes = bytes_done.load(std::memory_order_relaxed);
  double total_gbs = 0.0;
  int samples = 0;
  const auto interval_ns = static_cast<uint64_t>(opts.interval_s * 1e9);
  const auto end_ns = t_start + static_cast<uint64_t>(opts.duration_s * 1e9);
  while (true) {
    const uint64_t next = std::min(last_t + interval_ns, end_ns);
    while (now_ns() < next) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const uint64_t t = now_ns();
    const uint64_t bytes = bytes_done.load(std::memory_order_relaxed);
    const double gbs = static_cast<double>(bytes - last_bytes) / static_cast<double>(t - last_t);
    if (opts.sample) opts.sample(static_cast<double>(t - t_start) / 1e9, gbs);
    total_gbs += gbs;
    ++samples;
    last_t = t;
    last_bytes = bytes;
    if (t >= end_ns) break;
  }
  stop.store(true);
  for (auto& w : workers) w.join();
  return samples ? total_gbs / samples : 0.0;
}

json::Value to_json(const ThreadResult& r) {
  auto v = json::Value::object();
  v["threads"] = r.threads;
  auto cpus = json::Value::array();
  for (int c : r.cpus) cpus.push_back(c);
  v["cpus"] = std::move(cpus);
  auto kernels = json::Value::object();
  for (size_t ki = 0; ki < kKernels.size(); ++ki) {
    const auto& k = r.kernels[ki];
    auto j = json::Value::object();
    j["best_gbs"] = k.summary.max;
    j["median_gbs"] = k.summary.median;
    j["min_gbs"] = k.summary.min;
    j["spread_pct"] = k.summary.spread_pct;
    auto samples = json::Value::array();
    for (double s : k.samples_gbs) samples.push_back(s);
    j["samples_gbs"] = std::move(samples);
    j["counters"] = k.counters.available ? perfctr::to_json(k.counters) : json::Value(nullptr);
    kernels[kernel_name(kKernels[ki])] = std::move(j);
  }
  v["kernels"] = std::move(kernels);
  v["validated"] = r.validated;
  v["max_rel_error"] = r.max_rel_error;
  return v;
}

}  // namespace hwdiag::membw
