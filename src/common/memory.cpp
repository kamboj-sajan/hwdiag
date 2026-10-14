#include "common/memory.hpp"

#include <linux/mempolicy.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "common/sysfs.hpp"

namespace hwdiag {

const char* pages_name(Pages p) {
  switch (p) {
    case Pages::Default: return "default";
    case Pages::Small: return "4k";
    case Pages::Huge: return "huge";
  }
  return "unknown";
}

MappedBuffer::MappedBuffer(size_t bytes, Pages pages) : size_(bytes) {
  if (bytes == 0) throw std::invalid_argument("MappedBuffer: size 0");
  // Round up to whole huge pages and over-allocate by one so the start can
  // be aligned to 2 MiB; the slack is unmapped again below.
  const size_t rounded = (bytes + kHugePageSize - 1) / kHugePageSize * kHugePageSize;
  const size_t request = rounded + kHugePageSize;
  void* raw = mmap(nullptr, request, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) {
    throw std::runtime_error("mmap(" + std::to_string(request) + " bytes): " + std::strerror(errno));
  }
  const auto base = reinterpret_cast<uintptr_t>(raw);
  const uintptr_t aligned = (base + kHugePageSize - 1) & ~(uintptr_t{kHugePageSize} - 1);
  if (aligned > base) munmap(raw, aligned - base);
  const uintptr_t end = aligned + rounded;
  if (base + request > end) munmap(reinterpret_cast<void*>(end), base + request - end);
  ptr_ = reinterpret_cast<void*>(aligned);
  mapped_ = rounded;
  if (pages == Pages::Huge) {
    advice_ok_ = madvise(ptr_, mapped_, MADV_HUGEPAGE) == 0;
  } else if (pages == Pages::Small) {
    advice_ok_ = madvise(ptr_, mapped_, MADV_NOHUGEPAGE) == 0;
  }
}

MappedBuffer::~MappedBuffer() { release(); }

MappedBuffer::MappedBuffer(MappedBuffer&& other) noexcept
    : ptr_(other.ptr_), size_(other.size_), mapped_(other.mapped_), advice_ok_(other.advice_ok_) {
  other.ptr_ = nullptr;
  other.size_ = other.mapped_ = 0;
}

MappedBuffer& MappedBuffer::operator=(MappedBuffer&& other) noexcept {
  if (this != &other) {
    release();
    ptr_ = other.ptr_;
    size_ = other.size_;
    mapped_ = other.mapped_;
    advice_ok_ = other.advice_ok_;
    other.ptr_ = nullptr;
    other.size_ = other.mapped_ = 0;
  }
  return *this;
}

void MappedBuffer::release() {
  if (ptr_) munmap(ptr_, mapped_);
  ptr_ = nullptr;
}

std::optional<uint64_t> anon_huge_kib(const void* addr) {
  std::ifstream in("/proc/self/smaps");
  std::string line;
  bool in_mapping = false;
  const auto target = reinterpret_cast<uintptr_t>(addr);
  while (std::getline(in, line)) {
    unsigned long long start = 0, end = 0;
    // Mapping headers look like "7f12a0000000-7f12c0000000 rw-p ...".
    if (std::sscanf(line.c_str(), "%llx-%llx ", &start, &end) == 2 && line.find(' ') != std::string::npos &&
        line.find(':') > line.find(' ')) {
      in_mapping = target >= start && target < end;
      continue;
    }
    if (in_mapping && line.rfind("AnonHugePages:", 0) == 0) {
      unsigned long long kib = 0;
      if (std::sscanf(line.c_str() + 14, "%llu", &kib) == 1) return kib;
    }
  }
  return std::nullopt;
}

uint64_t mem_available_bytes(const std::string& root) {
  std::ifstream in(sysfs::under(root, "/proc/meminfo"));
  std::string line;
  while (std::getline(in, line)) {
    unsigned long long kib = 0;
    if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kib) == 1) return kib * 1024ull;
  }
  return 0;
}

bool bind_memory_to_node(int node, std::string* err) {
  constexpr int kMaxNodes = 1024;
  if (node < 0 || node >= kMaxNodes) {
    if (err) *err = "node " + std::to_string(node) + " out of range";
    return false;
  }
  std::vector<unsigned long> mask(kMaxNodes / (8 * sizeof(unsigned long)), 0);
  mask[static_cast<size_t>(node) / (8 * sizeof(unsigned long))] |= 1ul << (node % (8 * sizeof(unsigned long)));
  // maxnode is one more than the number of bits, a long-standing quirk of
  // the syscall that libnuma also follows.
  if (syscall(SYS_set_mempolicy, MPOL_BIND, mask.data(), kMaxNodes + 1) != 0) {
    if (err) *err = "set_mempolicy(MPOL_BIND, node " + std::to_string(node) + "): " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace hwdiag
