// memtest test patterns. Each pattern writes the buffer, then reads it back
// and logs every word that differs from what was written.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/json.hpp"

namespace hwdiag::memtest {

struct MemError {
  std::string pattern;
  size_t offset = 0;  // bytes from the start of the buffer
  uint64_t expected = 0;
  uint64_t actual = 0;
  uint64_t xor_mask() const { return expected ^ actual; }
};

// Keeps the first max_stored errors and counts all of them.
class ErrorLog {
 public:
  explicit ErrorLog(size_t max_stored = 1000) : max_stored_(max_stored) {}
  void add(const char* pattern, size_t offset, uint64_t expected, uint64_t actual);
  uint64_t total() const { return total_; }
  const std::vector<MemError>& stored() const { return errors_; }
  bool truncated() const { return total_ > errors_.size(); }

 private:
  size_t max_stored_;
  uint64_t total_ = 0;
  std::vector<MemError> errors_;
};

// --inject-fault <offset>:<bit>. offset is a byte offset aligned to 8 (one
// 64-bit word); bit is 0..63 within that word.
struct Fault {
  size_t offset = 0;
  unsigned bit = 0;
};
// Accepts decimal or 0x-prefixed offsets. Throws std::invalid_argument.
Fault parse_fault(const std::string& text);

struct Context {
  uint64_t* words = nullptr;
  size_t count = 0;  // number of 64-bit words
  ErrorLog* log = nullptr;
  std::optional<Fault> fault;
  uint64_t seed = 1;
  bool injected = false;  // reset before each pattern

  // Called by each pattern once, after its first write phase and before its
  // first check: flips the fault bit, simulating a cell that lost its value.
  void inject();
};

using PatternFn = void (*)(Context&);

struct Pattern {
  const char* name;
  const char* catches;  // the fault class it is designed for
  int sweeps;           // full-buffer reads + writes, for throughput
  PatternFn run;
};

const std::vector<Pattern>& all_patterns();
const Pattern* find_pattern(const std::string& name);

// The individual patterns.
void zeros(Context& c);
void ones(Context& c);
void checkerboard(Context& c);
void walking_ones(Context& c);
void walking_zeros(Context& c);
void address_in_address(Context& c);
void moving_inversions(Context& c);
void random_data(Context& c);

// Value the random pattern writes at word index i (splitmix64, counter based).
uint64_t random_word(uint64_t seed, size_t index);

inline constexpr uint64_t kCheckerA = 0x5555555555555555ull;
inline constexpr uint64_t kCheckerB = 0xAAAAAAAAAAAAAAAAull;

json::Value to_json(const MemError& e);

// Physical address of a virtual address via /proc/self/pagemap. Needs
// CAP_SYS_ADMIN; nullopt otherwise (the kernel reports PFN 0 to others).
std::optional<uint64_t> physical_address(const void* addr);

}  // namespace hwdiag::memtest
