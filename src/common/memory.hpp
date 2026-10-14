// Page-aligned anonymous buffers with control over transparent huge pages.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace hwdiag {

enum class Pages {
  Default,  // whatever the system THP setting gives
  Small,    // MADV_NOHUGEPAGE: 4 KiB pages even when THP is "always"
  Huge,     // MADV_HUGEPAGE before first touch
};

const char* pages_name(Pages p);

inline constexpr size_t kHugePageSize = 2u << 20;

// An mmap'd buffer, 2 MiB aligned (so THP can back it), released on
// destruction. Pages are not touched here: the caller decides who touches
// them first (that thread's NUMA node gets them).
class MappedBuffer {
 public:
  MappedBuffer() = default;
  // Throws std::runtime_error if mmap fails.
  MappedBuffer(size_t bytes, Pages pages);
  ~MappedBuffer();
  MappedBuffer(MappedBuffer&& other) noexcept;
  MappedBuffer& operator=(MappedBuffer&& other) noexcept;
  MappedBuffer(const MappedBuffer&) = delete;
  MappedBuffer& operator=(const MappedBuffer&) = delete;

  void* data() const { return ptr_; }
  size_t size() const { return size_; }
  size_t mapped_size() const { return mapped_; }
  // True when madvise() for the requested page size succeeded.
  bool advice_applied() const { return advice_ok_; }

 private:
  void release();
  void* ptr_ = nullptr;
  size_t size_ = 0;
  size_t mapped_ = 0;
  bool advice_ok_ = true;
};

// kB of AnonHugePages backing the mapping that starts at addr, from
// /proc/self/smaps. nullopt if the mapping can't be found.
std::optional<uint64_t> anon_huge_kib(const void* addr);

// MemAvailable from /proc/meminfo, in bytes (0 if unreadable).
uint64_t mem_available_bytes(const std::string& root = "/");

// Binds this thread's future allocations to one NUMA node with the
// set_mempolicy syscall (MPOL_BIND). False and err filled on failure.
bool bind_memory_to_node(int node, std::string* err = nullptr);

}  // namespace hwdiag
