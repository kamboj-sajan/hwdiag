// perfctr: hardware performance counters through perf_event_open.
//
//   CounterGroup g;      // opens the events (never throws)
//   g.start();
//   work();
//   Reading r = g.stop(); // r.available, r.value(Event::...), r.ipc(), ...
//
// When counters can't be used (no PMU in a VM, perf_event_paranoid too high,
// seccomp in a container) the group reports "unavailable" with a reason and
// start()/stop() do nothing. Callers carry on without counters.
#pragma once

#include <linux/perf_event.h>
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/json.hpp"

namespace hwdiag::perfctr {

enum class Event {
  Cycles,
  Instructions,
  CacheReferences,
  CacheMisses,
  BranchMisses,
  L1dReadMisses,
  DtlbReadMisses,
};

const char* event_name(Event e);
std::vector<Event> default_events();

// perf_event_attr for one event: user space only (exclude_kernel, exclude_hv),
// group read format with enabled/running times and ids.
perf_event_attr make_attr(Event e, bool leader);

// The system calls perfctr makes, replaceable in tests. They follow the libc
// convention: -1 and errno on failure.
struct SysOps {
  std::function<int(perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags)> open;
  std::function<int(int fd, unsigned long request, unsigned long arg)> ioctl;
  std::function<ssize_t(int fd, void* buf, size_t count)> read;
  std::function<int(int fd)> close;
};
SysOps real_sys_ops();

// errno values that mean "counters unavailable here" rather than a bug.
bool is_unavailable_errno(int err);
std::string describe_errno(int err);

// raw * enabled / running. Zero when the event never ran.
double scale_count(uint64_t raw, uint64_t enabled, uint64_t running);

struct EventCount {
  Event event = Event::Cycles;
  uint64_t raw = 0;
  double scaled = 0.0;
  uint64_t time_enabled = 0;
  uint64_t time_running = 0;
  double scale = 1.0;  // enabled / running for this event's group
};

struct Reading {
  bool available = false;
  std::string reason;            // why not, when unavailable
  std::vector<EventCount> counts;
  int groups = 0;
  bool multiplexed = false;      // some group ran for less time than it was enabled
  double max_scale = 1.0;
  std::vector<std::pair<Event, std::string>> dropped;  // events that could not be opened

  std::optional<double> value(Event e) const;  // scaled count
  std::optional<double> ipc() const;
  // Event count per 1,000 instructions.
  std::optional<double> per_kilo_instructions(Event e) const;
};

// Sums several readings of the same group (e.g. repetitions or threads).
Reading sum(const std::vector<Reading>& readings);

json::Value to_json(const Reading& r);

class CounterGroup {
 public:
  explicit CounterGroup(std::vector<Event> events = default_events(), SysOps ops = real_sys_ops(),
                        bool probe = true);
  ~CounterGroup();
  CounterGroup(const CounterGroup&) = delete;
  CounterGroup& operator=(const CounterGroup&) = delete;

  bool available() const { return available_; }
  const std::string& reason() const { return reason_; }
  int group_count() const { return static_cast<int>(groups_.size()); }
  std::vector<Event> active_events() const;
  // Events per group, in order; the first of each is its leader.
  std::vector<std::vector<Event>> layout() const;

  void start();
  Reading stop();

 private:
  struct Member {
    Event event;
    int fd = -1;
    uint64_t id = 0;
  };
  struct Group {
    std::vector<Member> members;  // members[0] is the leader
    uint64_t base_enabled = 0;
    uint64_t base_running = 0;
  };
  struct GroupRead {
    bool ok = false;
    uint64_t enabled = 0;
    uint64_t running = 0;
    std::vector<std::pair<uint64_t, uint64_t>> values;  // (id, value)
  };

  // Opens events in groups of at most max_size. Returns false if counters
  // are unavailable altogether (reason_ is set).
  bool open_groups(size_t max_size);
  GroupRead read_group(const Group& g) const;
  void close_all();
  // Runs a short busy loop and returns the size of the first group that
  // was enabled but never scheduled, or 0 if every group ran.
  size_t probe_unschedulable();

  std::vector<Event> events_;
  SysOps ops_;
  std::vector<Group> groups_;
  std::vector<std::pair<Event, std::string>> dropped_;
  bool available_ = false;
  bool running_ = false;
  std::string reason_;
};

}  // namespace hwdiag::perfctr
