#include "memtest/patterns.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <stdexcept>

namespace hwdiag::memtest {

namespace {

// Stops the compiler from assuming it knows what memory holds, so a check
// phase really reads back what the write phase stored.
inline void compiler_barrier() { asm volatile("" ::: "memory"); }

template <typename F>
void fill(Context& c, F value) {
  uint64_t* w = c.words;
  for (size_t i = 0; i < c.count; ++i) w[i] = value(i);
  compiler_barrier();
}

template <typename F>
void check(Context& c, const char* name, F expected) {
  compiler_barrier();
  const uint64_t* w = c.words;
  for (size_t i = 0; i < c.count; ++i) {
    const uint64_t e = expected(i);
    const uint64_t v = w[i];
    if (v != e) c.log->add(name, i * sizeof(uint64_t), e, v);
  }
}

inline uint64_t rotl(uint64_t x, unsigned k) { return k == 0 ? x : (x << k) | (x >> (64 - k)); }

}  // namespace

void ErrorLog::add(const char* pattern, size_t offset, uint64_t expected, uint64_t actual) {
  ++total_;
  if (errors_.size() < max_stored_) errors_.push_back({pattern, offset, expected, actual});
}

Fault parse_fault(const std::string& text) {
  const auto colon = text.find(':');
  if (colon == std::string::npos) throw std::invalid_argument("--inject-fault wants <offset>:<bit>");
  const std::string off = text.substr(0, colon), bit = text.substr(colon + 1);
  char* end = nullptr;
  const bool hex = off.size() > 2 && off[0] == '0' && (off[1] == 'x' || off[1] == 'X');
  const unsigned long long o = std::strtoull(off.c_str(), &end, hex ? 16 : 10);
  if (off.empty() || *end != '\0') throw std::invalid_argument("--inject-fault: bad offset '" + off + "'");
  const unsigned long b = std::strtoul(bit.c_str(), &end, 10);
  if (bit.empty() || *end != '\0' || b > 63) throw std::invalid_argument("--inject-fault: bit must be 0..63");
  if (o % sizeof(uint64_t) != 0) throw std::invalid_argument("--inject-fault: offset must be a multiple of 8");
  return Fault{static_cast<size_t>(o), static_cast<unsigned>(b)};
}

void Context::inject() {
  if (!fault || injected) return;
  injected = true;
  const size_t index = fault->offset / sizeof(uint64_t);
  if (index >= count) return;
  volatile uint64_t* w = words + index;
  *w = *w ^ (uint64_t{1} << fault->bit);
}

// Stuck-at faults: every bit must hold 0.
void zeros(Context& c) {
  fill(c, [](size_t) { return uint64_t{0}; });
  c.inject();
  check(c, "zeros", [](size_t) { return uint64_t{0}; });
}

// Stuck-at faults: every bit must hold 1.
void ones(Context& c) {
  fill(c, [](size_t) { return ~uint64_t{0}; });
  c.inject();
  check(c, "ones", [](size_t) { return ~uint64_t{0}; });
}

// Neighbouring bits (within a word and between words) hold opposite values:
// catches bits coupled to their neighbours and leakage between cells.
void checkerboard(Context& c) {
  for (int inverted = 0; inverted < 2; ++inverted) {
    auto value = [inverted](size_t i) {
      const uint64_t v = (i % 2 == 0) ? kCheckerA : kCheckerB;
      return inverted ? ~v : v;
    };
    fill(c, value);
    c.inject();
    check(c, "checkerboard", value);
  }
}

// One set bit per word, in a different position in neighbouring words, moved
// through all 64 positions: every data line carries a lone 1.
void walking_ones(Context& c) {
  for (unsigned k = 0; k < 64; ++k) {
    auto value = [k](size_t i) { return rotl(uint64_t{1}, static_cast<unsigned>((i + k) % 64)); };
    fill(c, value);
    c.inject();
    check(c, "walking_ones", value);
  }
}

// The inverse: a lone 0 walks through every bit position.
void walking_zeros(Context& c) {
  for (unsigned k = 0; k < 64; ++k) {
    auto value = [k](size_t i) { return ~rotl(uint64_t{1}, static_cast<unsigned>((i + k) % 64)); };
    fill(c, value);
    c.inject();
    check(c, "walking_zeros", value);
  }
}

// Each word stores its own address, then the complement: if an address line
// is stuck or two addresses alias, a word reads back another word's address.
void address_in_address(Context& c) {
  const auto base = reinterpret_cast<uintptr_t>(c.words);
  for (int inverted = 0; inverted < 2; ++inverted) {
    auto value = [base, inverted](size_t i) {
      const uint64_t a = base + i * sizeof(uint64_t);
      return inverted ? ~a : a;
    };
    fill(c, value);
    c.inject();
    check(c, "address_in_address", value);
  }
}

// Write p upward; then upward, check p and write ~p; then downward, check
// ~p and write p. Checking each cell just before overwriting it, in both
// directions, catches coupling faults where writing one cell disturbs
// another (an earlier or later address).
void moving_inversions(Context& c) {
  for (const uint64_t p : {uint64_t{0}, kCheckerA}) {
    fill(c, [p](size_t) { return p; });
    c.inject();
    compiler_barrier();
    uint64_t* w = c.words;
    for (size_t i = 0; i < c.count; ++i) {
      if (w[i] != p) c.log->add("moving_inversions", i * sizeof(uint64_t), p, w[i]);
      w[i] = ~p;
    }
    compiler_barrier();
    for (size_t i = c.count; i-- > 0;) {
      if (w[i] != ~p) c.log->add("moving_inversions", i * sizeof(uint64_t), ~p, w[i]);
      w[i] = p;
    }
    compiler_barrier();
  }
}

uint64_t random_word(uint64_t seed, size_t index) {
  uint64_t z = seed + (static_cast<uint64_t>(index) + 1) * 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Seeded random data, then its complement: no regular structure, so it
// catches pattern-sensitive faults the regular patterns miss.
void random_data(Context& c) {
  const uint64_t seed = c.seed;
  for (int inverted = 0; inverted < 2; ++inverted) {
    auto value = [seed, inverted](size_t i) {
      const uint64_t v = random_word(seed, i);
      return inverted ? ~v : v;
    };
    fill(c, value);
    c.inject();
    check(c, "random", value);
  }
}

const std::vector<Pattern>& all_patterns() {
  static const std::vector<Pattern> patterns = {
      {"zeros", "bits stuck at 1", 2, zeros},
      {"ones", "bits stuck at 0", 2, ones},
      {"checkerboard", "coupling between neighbouring bits and cells", 4, checkerboard},
      {"walking_ones", "data line faults, bits stuck at 0", 128, walking_ones},
      {"walking_zeros", "data line faults, bits stuck at 1", 128, walking_zeros},
      {"address_in_address", "address line faults and aliasing", 4, address_in_address},
      {"moving_inversions", "coupling faults between cells, in either direction", 10, moving_inversions},
      {"random", "pattern-sensitive faults", 4, random_data},
  };
  return patterns;
}

const Pattern* find_pattern(const std::string& name) {
  for (const auto& p : all_patterns()) {
    if (name == p.name) return &p;
  }
  return nullptr;
}

namespace {

std::string hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
  return buf;
}

}  // namespace

json::Value to_json(const MemError& e) {
  auto v = json::Value::object();
  v["pattern"] = e.pattern;
  v["offset"] = static_cast<uint64_t>(e.offset);
  v["offset_hex"] = hex(e.offset);
  v["expected"] = hex(e.expected);
  v["actual"] = hex(e.actual);
  v["xor_mask"] = hex(e.xor_mask());
  auto bits = json::Value::array();
  for (unsigned b = 0; b < 64; ++b) {
    if ((e.xor_mask() >> b) & 1u) bits.push_back(b);
  }
  v["flipped_bits"] = std::move(bits);
  return v;
}

std::optional<uint64_t> physical_address(const void* addr) {
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return std::nullopt;
  const int fd = open("/proc/self/pagemap", O_RDONLY);
  if (fd < 0) return std::nullopt;
  const auto vaddr = reinterpret_cast<uintptr_t>(addr);
  uint64_t entry = 0;
  const off_t pos = static_cast<off_t>(vaddr / static_cast<uintptr_t>(page) * sizeof(entry));
  const ssize_t n = pread(fd, &entry, sizeof entry, pos);
  close(fd);
  if (n != static_cast<ssize_t>(sizeof entry)) return std::nullopt;
  const bool present = (entry >> 63) & 1u;
  const uint64_t pfn = entry & ((uint64_t{1} << 55) - 1);
  if (!present || pfn == 0) return std::nullopt;
  return pfn * static_cast<uint64_t>(page) + vaddr % static_cast<uintptr_t>(page);
}

}  // namespace hwdiag::memtest
