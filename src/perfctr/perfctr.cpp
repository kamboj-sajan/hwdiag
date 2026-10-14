#include "perfctr/perfctr.hpp"

#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace hwdiag::perfctr {

const char* event_name(Event e) {
  switch (e) {
    case Event::Cycles: return "cycles";
    case Event::Instructions: return "instructions";
    case Event::CacheReferences: return "cache_references";
    case Event::CacheMisses: return "cache_misses";
    case Event::BranchMisses: return "branch_misses";
    case Event::L1dReadMisses: return "l1d_read_misses";
    case Event::DtlbReadMisses: return "dtlb_read_misses";
  }
  return "unknown";
}

std::vector<Event> default_events() {
  return {Event::Cycles,       Event::Instructions,  Event::CacheReferences, Event::CacheMisses,
          Event::BranchMisses, Event::L1dReadMisses, Event::DtlbReadMisses};
}

perf_event_attr make_attr(Event e, bool leader) {
  perf_event_attr attr;
  std::memset(&attr, 0, sizeof attr);
  attr.size = sizeof attr;
  auto hw_cache = [](uint64_t cache) {
    return cache | (uint64_t{PERF_COUNT_HW_CACHE_OP_READ} << 8) | (uint64_t{PERF_COUNT_HW_CACHE_RESULT_MISS} << 16);
  };
  switch (e) {
    case Event::Cycles: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CPU_CYCLES; break;
    case Event::Instructions: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_INSTRUCTIONS; break;
    case Event::CacheReferences: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CACHE_REFERENCES; break;
    case Event::CacheMisses: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_CACHE_MISSES; break;
    case Event::BranchMisses: attr.type = PERF_TYPE_HARDWARE; attr.config = PERF_COUNT_HW_BRANCH_MISSES; break;
    case Event::L1dReadMisses: attr.type = PERF_TYPE_HW_CACHE; attr.config = hw_cache(PERF_COUNT_HW_CACHE_L1D); break;
    case Event::DtlbReadMisses: attr.type = PERF_TYPE_HW_CACHE; attr.config = hw_cache(PERF_COUNT_HW_CACHE_DTLB); break;
  }
  // Only the leader starts disabled; members follow the leader.
  attr.disabled = leader ? 1 : 0;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;
  return attr;
}

SysOps real_sys_ops() {
  SysOps ops;
  // glibc has no perf_event_open wrapper.
  ops.open = [](perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return static_cast<int>(syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags));
  };
  ops.ioctl = [](int fd, unsigned long request, unsigned long arg) { return ::ioctl(fd, request, arg); };
  ops.read = [](int fd, void* buf, size_t count) { return ::read(fd, buf, count); };
  ops.close = [](int fd) { return ::close(fd); };
  return ops;
}

bool is_unavailable_errno(int err) {
  switch (err) {
    case EACCES:
    case EPERM:
    case ENOENT:
    case EOPNOTSUPP:
    case ENODEV:
    case ENOSYS:
      return true;
    default:
      return false;
  }
}

std::string describe_errno(int err) {
  std::string name;
  std::string hint;
  switch (err) {
    case EACCES: name = "EACCES"; hint = "kernel.perf_event_paranoid is too high; try: sudo sysctl kernel.perf_event_paranoid=1"; break;
    case EPERM: name = "EPERM"; hint = "not permitted (perf_event_paranoid, or a seccomp filter in a container)"; break;
    case ENOENT: name = "ENOENT"; hint = "event not supported here (VMs often expose no CPU PMU)"; break;
    case EOPNOTSUPP: name = "EOPNOTSUPP"; hint = "the PMU does not support this event or mode"; break;
    case ENODEV: name = "ENODEV"; hint = "no PMU device"; break;
    case ENOSYS: name = "ENOSYS"; hint = "kernel built without perf events"; break;
    default: name = "errno " + std::to_string(err); hint = std::strerror(err); break;
  }
  return "perf_event_open failed: " + name + ": " + hint;
}

double scale_count(uint64_t raw, uint64_t enabled, uint64_t running) {
  if (running == 0) return 0.0;
  if (enabled <= running) return static_cast<double>(raw);
  return static_cast<double>(raw) * static_cast<double>(enabled) / static_cast<double>(running);
}

std::optional<double> Reading::value(Event e) const {
  if (!available) return std::nullopt;
  for (const auto& c : counts) {
    if (c.event == e) {
      if (c.time_running == 0) return std::nullopt;
      return c.scaled;
    }
  }
  return std::nullopt;
}

std::optional<double> Reading::ipc() const {
  auto ins = value(Event::Instructions), cyc = value(Event::Cycles);
  if (!ins || !cyc || *cyc <= 0.0) return std::nullopt;
  return *ins / *cyc;
}

std::optional<double> Reading::per_kilo_instructions(Event e) const {
  auto ins = value(Event::Instructions), v = value(e);
  if (!ins || !v || *ins <= 0.0) return std::nullopt;
  return *v * 1000.0 / *ins;
}

Reading sum(const std::vector<Reading>& readings) {
  Reading total;
  if (readings.empty()) {
    total.reason = "no readings";
    return total;
  }
  total.available = true;
  for (const auto& r : readings) {
    if (!r.available) return r;  // one unavailable reading makes the sum unavailable
    total.groups = std::max(total.groups, r.groups);
    total.multiplexed |= r.multiplexed;
    total.max_scale = std::max(total.max_scale, r.max_scale);
    if (total.dropped.empty()) total.dropped = r.dropped;
    for (const auto& c : r.counts) {
      auto it = std::find_if(total.counts.begin(), total.counts.end(),
                             [&](const EventCount& t) { return t.event == c.event; });
      if (it == total.counts.end()) {
        total.counts.push_back(c);
      } else {
        it->raw += c.raw;
        it->scaled += c.scaled;
        it->time_enabled += c.time_enabled;
        it->time_running += c.time_running;
        it->scale = it->time_running ? static_cast<double>(it->time_enabled) / static_cast<double>(it->time_running) : 0.0;
      }
    }
  }
  return total;
}

json::Value to_json(const Reading& r) {
  auto v = json::Value::object();
  v["available"] = r.available;
  v["reason"] = r.available ? json::Value(nullptr) : json::Value(r.reason);
  if (!r.available) return v;
  v["groups"] = r.groups;
  v["multiplexed"] = r.multiplexed;
  v["max_scale"] = r.max_scale;
  auto events = json::Value::object();
  for (const auto& c : r.counts) {
    auto e = json::Value::object();
    e["raw"] = c.raw;
    e["scaled"] = c.time_running ? json::Value(c.scaled) : json::Value(nullptr);
    e["time_enabled_ns"] = c.time_enabled;
    e["time_running_ns"] = c.time_running;
    e["scale"] = c.scale;
    events[event_name(c.event)] = std::move(e);
  }
  v["events"] = std::move(events);
  auto opt = [](std::optional<double> x) { return x ? json::Value(*x) : json::Value(nullptr); };
  auto derived = json::Value::object();
  derived["ipc"] = opt(r.ipc());
  derived["cache_misses_pki"] = opt(r.per_kilo_instructions(Event::CacheMisses));
  derived["branch_misses_pki"] = opt(r.per_kilo_instructions(Event::BranchMisses));
  derived["l1d_read_misses_pki"] = opt(r.per_kilo_instructions(Event::L1dReadMisses));
  derived["dtlb_read_misses_pki"] = opt(r.per_kilo_instructions(Event::DtlbReadMisses));
  auto refs = r.value(Event::CacheReferences), misses = r.value(Event::CacheMisses);
  derived["cache_miss_ratio"] = refs && misses && *refs > 0 ? json::Value(*misses / *refs) : json::Value(nullptr);
  v["derived"] = std::move(derived);
  auto dropped = json::Value::array();
  for (const auto& [e, why] : r.dropped) {
    auto d = json::Value::object();
    d["event"] = event_name(e);
    d["reason"] = why;
    dropped.push_back(std::move(d));
  }
  v["dropped"] = std::move(dropped);
  return v;
}

CounterGroup::CounterGroup(std::vector<Event> events, SysOps ops, bool probe)
    : events_(std::move(events)), ops_(std::move(ops)) {
  if (events_.empty()) {
    reason_ = "no events requested";
    return;
  }
  size_t max_size = events_.size();
  while (true) {
    if (!open_groups(max_size)) return;  // unavailable; reason_ is set
    if (!probe) break;
    // A group that doesn't fit the PMU is never scheduled: time_running
    // stays 0 and every count reads 0. Split it and try again.
    const size_t stuck = probe_unschedulable();
    if (stuck == 0) break;
    if (stuck <= 1) {
      close_all();
      available_ = false;
      reason_ = "counters open but never scheduled (all PMU counters in use?)";
      return;
    }
    close_all();
    max_size = stuck - 1;
  }
}

CounterGroup::~CounterGroup() { close_all(); }

void CounterGroup::close_all() {
  for (auto& g : groups_) {
    for (auto& m : g.members) {
      if (m.fd >= 0) ops_.close(m.fd);
    }
  }
  groups_.clear();
}

bool CounterGroup::open_groups(size_t max_size) {
  groups_.clear();
  dropped_.clear();
  available_ = false;
  for (Event e : events_) {
    const bool need_new = groups_.empty() || groups_.back().members.size() >= max_size;
    if (!need_new) {
      perf_event_attr attr = make_attr(e, false);
      errno = 0;
      const int fd = ops_.open(&attr, 0, -1, groups_.back().members.front().fd, 0);
      if (fd >= 0) {
        groups_.back().members.push_back({e, fd, 0});
        continue;
      }
      const int err = errno;
      // EINVAL/ENOSPC when joining a group: the group can never fit on the
      // PMU (x86 validates this at open). Start a new group with this event.
      if (err != EINVAL && err != ENOSPC) {
        dropped_.emplace_back(e, describe_errno(err));
        continue;
      }
    }
    perf_event_attr attr = make_attr(e, true);
    errno = 0;
    const int fd = ops_.open(&attr, 0, -1, -1, 0);
    if (fd < 0) {
      const int err = errno;
      if (groups_.empty() && e == events_.front()) {
        // The first leader (cycles) failing means no usable counters at all.
        reason_ = describe_errno(err);
        return false;
      }
      dropped_.emplace_back(e, describe_errno(err));
      continue;
    }
    groups_.push_back(Group{{{e, fd, 0}}, 0, 0});
  }
  for (auto& g : groups_) {
    for (auto& m : g.members) {
      uint64_t id = 0;
      if (ops_.ioctl(m.fd, PERF_EVENT_IOC_ID, reinterpret_cast<unsigned long>(&id)) == 0) m.id = id;
    }
  }
  available_ = !groups_.empty();
  if (!available_) reason_ = "no events could be opened";
  return available_;
}

CounterGroup::GroupRead CounterGroup::read_group(const Group& g) const {
  GroupRead out;
  // Layout for PERF_FORMAT_GROUP | TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING | ID:
  // nr, time_enabled, time_running, then nr pairs of (value, id).
  std::vector<uint64_t> buf(3 + 2 * g.members.size());
  const ssize_t n = ops_.read(g.members.front().fd, buf.data(), buf.size() * sizeof(uint64_t));
  if (n < static_cast<ssize_t>(3 * sizeof(uint64_t))) return out;
  const uint64_t nr = buf[0];
  if (nr > g.members.size() || static_cast<size_t>(n) < (3 + 2 * nr) * sizeof(uint64_t)) return out;
  out.ok = true;
  out.enabled = buf[1];
  out.running = buf[2];
  for (uint64_t i = 0; i < nr; ++i) out.values.emplace_back(buf[4 + 2 * i], buf[3 + 2 * i]);
  return out;
}

std::vector<Event> CounterGroup::active_events() const {
  std::vector<Event> out;
  for (const auto& g : groups_) {
    for (const auto& m : g.members) out.push_back(m.event);
  }
  return out;
}

std::vector<std::vector<Event>> CounterGroup::layout() const {
  std::vector<std::vector<Event>> out;
  for (const auto& g : groups_) {
    std::vector<Event> evs;
    for (const auto& m : g.members) evs.push_back(m.event);
    out.push_back(evs);
  }
  return out;
}

void CounterGroup::start() {
  if (!available_) return;
  for (auto& g : groups_) {
    const int leader = g.members.front().fd;
    ops_.ioctl(leader, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    // RESET zeroes the counts but not the enabled/running times, so keep a
    // baseline and use deltas.
    const GroupRead base = read_group(g);
    g.base_enabled = base.enabled;
    g.base_running = base.running;
  }
  for (auto& g : groups_) ops_.ioctl(g.members.front().fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  running_ = true;
}

Reading CounterGroup::stop() {
  Reading r;
  if (!available_) {
    r.reason = reason_;
    return r;
  }
  for (auto& g : groups_) ops_.ioctl(g.members.front().fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
  running_ = false;
  r.available = true;
  r.groups = static_cast<int>(groups_.size());
  r.dropped = dropped_;
  for (const auto& g : groups_) {
    const GroupRead gr = read_group(g);
    if (!gr.ok) {
      r.available = false;
      r.reason = "reading the counter group failed";
      return r;
    }
    const uint64_t enabled = gr.enabled - std::min(gr.enabled, g.base_enabled);
    const uint64_t running = gr.running - std::min(gr.running, g.base_running);
    const double scale = running ? static_cast<double>(enabled) / static_cast<double>(running) : 0.0;
    if (running < enabled) r.multiplexed = true;
    r.max_scale = std::max(r.max_scale, scale);
    for (const auto& m : g.members) {
      EventCount c;
      c.event = m.event;
      for (const auto& [id, value] : gr.values) {
        if (id == m.id) c.raw = value;
      }
      c.time_enabled = enabled;
      c.time_running = running;
      c.scale = scale;
      c.scaled = scale_count(c.raw, enabled, running);
      r.counts.push_back(c);
    }
  }
  return r;
}

size_t CounterGroup::probe_unschedulable() {
  start();
  volatile uint64_t x = 0;
  for (int i = 0; i < 2'000'000; ++i) x = x + static_cast<uint64_t>(i);
  stop();
  for (const auto& g : groups_) {
    const GroupRead gr = read_group(g);
    if (!gr.ok) continue;
    const uint64_t enabled = gr.enabled - std::min(gr.enabled, g.base_enabled);
    const uint64_t running = gr.running - std::min(gr.running, g.base_running);
    if (enabled > 0 && running == 0) return g.members.size();
  }
  return 0;
}

}  // namespace hwdiag::perfctr
