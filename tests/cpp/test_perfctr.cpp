#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <set>

#include "perfctr/perfctr.hpp"

using namespace hwdiag::perfctr;

namespace {

Event event_of(const perf_event_attr& attr) {
  for (Event e : default_events()) {
    const perf_event_attr ref = make_attr(e, true);
    if (ref.type == attr.type && ref.config == attr.config) return e;
  }
  ADD_FAILURE() << "unknown attr";
  return Event::Cycles;
}

// A pretend kernel behind SysOps: tracks fds, groups and counts, and can
// refuse events, cap group sizes or leave groups unscheduled.
class FakeKernel {
 public:
  std::map<Event, int> fail_errno;   // perf_event_open fails for these events
  size_t max_group_size = 100;       // joining a fuller group returns EINVAL
  size_t schedulable_size = 100;     // groups larger than this never run
  double run_fraction = 1.0;         // < 1 simulates multiplexing
  std::map<Event, uint64_t> per_period = {
      {Event::Cycles, 3000},      {Event::Instructions, 1500}, {Event::CacheReferences, 100},
      {Event::CacheMisses, 20},   {Event::BranchMisses, 5},    {Event::L1dReadMisses, 60},
      {Event::DtlbReadMisses, 2}};

  SysOps ops() {
    SysOps o;
    o.open = [this](perf_event_attr* attr, pid_t, int, int group_fd, unsigned long) { return open(*attr, group_fd); };
    o.ioctl = [this](int fd, unsigned long req, unsigned long arg) { return ioctl(fd, req, arg); };
    o.read = [this](int fd, void* buf, size_t n) { return read(fd, buf, n); };
    o.close = [this](int fd) {
      closed.insert(fd);
      return 0;
    };
    return o;
  }

  std::set<int> closed;
  int opened = 0;

 private:
  struct Fd {
    Event event;
    int leader;
    uint64_t value = 0;
  };
  struct Group {
    std::vector<int> fds;
    bool enabled = false;
    uint64_t time_enabled = 0;
    uint64_t time_running = 0;
  };
  std::map<int, Fd> fds_;
  std::map<int, Group> groups_;
  int next_fd_ = 100;

  int open(const perf_event_attr& attr, int group_fd) {
    const Event e = event_of(attr);
    if (auto it = fail_errno.find(e); it != fail_errno.end()) {
      errno = it->second;
      return -1;
    }
    if (group_fd >= 0 && groups_[group_fd].fds.size() >= max_group_size) {
      errno = EINVAL;
      return -1;
    }
    const int fd = next_fd_++;
    ++opened;
    const int leader = group_fd >= 0 ? group_fd : fd;
    fds_[fd] = {e, leader};
    groups_[leader].fds.push_back(fd);
    return fd;
  }

  int ioctl(int fd, unsigned long req, unsigned long arg) {
    if (req == PERF_EVENT_IOC_ID) {
      *reinterpret_cast<uint64_t*>(arg) = static_cast<uint64_t>(fd) * 10;
      return 0;
    }
    Group& g = groups_.at(fds_.at(fd).leader);
    if (req == PERF_EVENT_IOC_RESET) {
      for (int f : g.fds) fds_[f].value = 0;
    } else if (req == PERF_EVENT_IOC_ENABLE) {
      g.enabled = true;
    } else if (req == PERF_EVENT_IOC_DISABLE && g.enabled) {
      // Each enable/disable period lasts 1000 ns of enabled time.
      g.enabled = false;
      g.time_enabled += 1000;
      const bool runs = g.fds.size() <= schedulable_size;
      const auto ran = runs ? static_cast<uint64_t>(1000 * run_fraction) : 0;
      g.time_running += ran;
      for (int f : g.fds) fds_[f].value += runs ? static_cast<uint64_t>(per_period[fds_[f].event] * run_fraction) : 0;
    }
    return 0;
  }

  ssize_t read(int fd, void* buf, size_t n) {
    const Group& g = groups_.at(fd);
    std::vector<uint64_t> out = {g.fds.size(), g.time_enabled, g.time_running};
    for (int f : g.fds) {
      out.push_back(fds_.at(f).value);
      out.push_back(static_cast<uint64_t>(f) * 10);
    }
    const size_t bytes = out.size() * sizeof(uint64_t);
    if (n < bytes) {
      errno = ENOSPC;
      return -1;
    }
    std::memcpy(buf, out.data(), bytes);
    return static_cast<ssize_t>(bytes);
  }
};

}  // namespace

TEST(Perfctr, ScaleCountCorrectsForMultiplexing) {
  EXPECT_DOUBLE_EQ(scale_count(1000, 200, 100), 2000.0);
  EXPECT_DOUBLE_EQ(scale_count(1000, 100, 100), 1000.0);
  EXPECT_DOUBLE_EQ(scale_count(1000, 300, 100), 3000.0);
  EXPECT_DOUBLE_EQ(scale_count(1000, 100, 0), 0.0);
}

TEST(Perfctr, UnavailableErrnosAreRecognised) {
  for (int err : {EACCES, EPERM, ENOENT, EOPNOTSUPP, ENODEV, ENOSYS}) EXPECT_TRUE(is_unavailable_errno(err)) << err;
  EXPECT_FALSE(is_unavailable_errno(EINVAL));
  EXPECT_NE(describe_errno(EACCES).find("perf_event_paranoid"), std::string::npos);
}

class PerfctrUnavailable : public ::testing::TestWithParam<int> {};

TEST_P(PerfctrUnavailable, ReportsUnavailableWithoutCrashing) {
  FakeKernel k;
  for (Event e : default_events()) k.fail_errno[e] = GetParam();
  CounterGroup g(default_events(), k.ops());
  EXPECT_FALSE(g.available());
  EXPECT_NE(g.reason().find("perf_event_open failed"), std::string::npos);
  g.start();  // no-ops
  const Reading r = g.stop();
  EXPECT_FALSE(r.available);
  EXPECT_EQ(r.reason, g.reason());
  EXPECT_FALSE(r.ipc().has_value());
  EXPECT_FALSE(r.value(Event::Cycles).has_value());
  EXPECT_EQ(k.opened, 0);
  const auto j = to_json(r);
  EXPECT_EQ(j.dump(), "{\"available\":false,\"reason\":" + hwdiag::json::quote(g.reason()) + "}");
}

INSTANTIATE_TEST_SUITE_P(Errnos, PerfctrUnavailable,
                         ::testing::Values(EACCES, EPERM, ENOENT, EOPNOTSUPP, ENODEV, ENOSYS));

TEST(Perfctr, OneGroupWhenEverythingFits) {
  FakeKernel k;
  CounterGroup g(default_events(), k.ops());
  ASSERT_TRUE(g.available());
  EXPECT_EQ(g.group_count(), 1);
  EXPECT_EQ(g.layout().front().front(), Event::Cycles);  // cycles leads
  g.start();
  const Reading r = g.stop();
  ASSERT_TRUE(r.available);
  EXPECT_FALSE(r.multiplexed);
  EXPECT_DOUBLE_EQ(*r.value(Event::Cycles), 3000.0);
  EXPECT_DOUBLE_EQ(*r.value(Event::Instructions), 1500.0);
  EXPECT_DOUBLE_EQ(*r.ipc(), 0.5);
  EXPECT_DOUBLE_EQ(*r.per_kilo_instructions(Event::CacheMisses), 20.0 * 1000 / 1500);
}

TEST(Perfctr, FailingNonLeaderEventIsDroppedAndTheRestKept) {
  FakeKernel k;
  k.fail_errno[Event::DtlbReadMisses] = ENOENT;
  CounterGroup g(default_events(), k.ops());
  ASSERT_TRUE(g.available());
  const auto active = g.active_events();
  EXPECT_EQ(active.size(), 6u);
  EXPECT_EQ(std::count(active.begin(), active.end(), Event::DtlbReadMisses), 0);
  g.start();
  const Reading r = g.stop();
  ASSERT_TRUE(r.available);
  ASSERT_EQ(r.dropped.size(), 1u);
  EXPECT_EQ(r.dropped[0].first, Event::DtlbReadMisses);
  EXPECT_NE(r.dropped[0].second.find("ENOENT"), std::string::npos);
  EXPECT_FALSE(r.value(Event::DtlbReadMisses).has_value());
  EXPECT_DOUBLE_EQ(*r.value(Event::L1dReadMisses), 60.0);
}

TEST(Perfctr, GroupThatCannotBeOpenedWholeIsSplit) {
  // Like x86 rejecting a 5th general-purpose event with EINVAL.
  FakeKernel k;
  k.max_group_size = 4;
  CounterGroup g(default_events(), k.ops());
  ASSERT_TRUE(g.available());
  const auto layout = g.layout();
  ASSERT_EQ(layout.size(), 2u);
  EXPECT_EQ(layout[0].size(), 4u);
  EXPECT_EQ(layout[1].front(), Event::BranchMisses);  // first event of the new group leads it
  EXPECT_EQ(g.active_events().size(), 7u);
}

TEST(Perfctr, GroupThatIsNeverScheduledIsSplitAfterTheProbe) {
  // Opens fine but needs more counters than the PMU has: time_running stays 0.
  FakeKernel k;
  k.schedulable_size = 3;
  CounterGroup g(default_events(), k.ops());
  ASSERT_TRUE(g.available());
  for (const auto& group : g.layout()) EXPECT_LE(group.size(), 3u);
  EXPECT_EQ(g.active_events().size(), 7u);
  EXPECT_GT(k.closed.size(), 0u);  // the first, oversized group was closed
  g.start();
  const Reading r = g.stop();
  EXPECT_DOUBLE_EQ(*r.value(Event::DtlbReadMisses), 2.0);
}

TEST(Perfctr, NothingSchedulableMeansUnavailable) {
  FakeKernel k;
  k.schedulable_size = 0;
  CounterGroup g(default_events(), k.ops());
  EXPECT_FALSE(g.available());
  EXPECT_NE(g.reason().find("never scheduled"), std::string::npos);
}

TEST(Perfctr, MultiplexedCountsAreScaledUp) {
  FakeKernel k;
  k.run_fraction = 0.5;  // the group ran for half the time it was enabled
  CounterGroup g(default_events(), k.ops(), /*probe=*/false);
  ASSERT_TRUE(g.available());
  g.start();
  const Reading r = g.stop();
  ASSERT_TRUE(r.available);
  EXPECT_TRUE(r.multiplexed);
  EXPECT_DOUBLE_EQ(r.max_scale, 2.0);
  // 1500 * 0.5 counted, scaled by enabled/running = 2 back to 1500.
  EXPECT_DOUBLE_EQ(*r.value(Event::Cycles), 3000.0);
  EXPECT_EQ(r.counts[0].raw, 1500u);
}

TEST(Perfctr, StartUsesDeltasSoRepeatedMeasurementsDontAccumulate) {
  FakeKernel k;
  CounterGroup g(default_events(), k.ops(), false);
  for (int i = 0; i < 3; ++i) {
    g.start();
    const Reading r = g.stop();
    EXPECT_DOUBLE_EQ(*r.value(Event::Cycles), 3000.0);
    EXPECT_EQ(r.counts[0].time_enabled, 1000u);
  }
}

TEST(Perfctr, SumAddsReadings) {
  FakeKernel k;
  CounterGroup g(default_events(), k.ops(), false);
  std::vector<Reading> rs;
  for (int i = 0; i < 4; ++i) {
    g.start();
    rs.push_back(g.stop());
  }
  const Reading total = sum(rs);
  ASSERT_TRUE(total.available);
  EXPECT_DOUBLE_EQ(*total.value(Event::Cycles), 12000.0);
  EXPECT_DOUBLE_EQ(*total.ipc(), 0.5);
  EXPECT_FALSE(sum({}).available);
}

TEST(Perfctr, ClosesEveryFdOnDestruction) {
  FakeKernel k;
  { CounterGroup g(default_events(), k.ops(), false); }
  EXPECT_EQ(static_cast<int>(k.closed.size()), k.opened);
}

TEST(Perfctr, RealCountersEitherWorkOrSayWhy) {
  // On CI runners and in VMs this is usually unavailable; on real hardware
  // with perf_event_paranoid <= 2 it counts. Either way it must not crash.
  CounterGroup g;
  g.start();
  volatile uint64_t x = 0;
  for (int i = 0; i < 100000; ++i) x = x + 1;
  const Reading r = g.stop();
  if (g.available()) {
    ASSERT_TRUE(r.available);
    EXPECT_GT(r.value(Event::Instructions).value_or(0.0), 100000.0);
  } else {
    EXPECT_FALSE(r.reason.empty());
  }
}
