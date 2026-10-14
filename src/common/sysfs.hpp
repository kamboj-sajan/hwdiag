// Helpers for reading /proc and /sys. Every function takes a root directory
// so tests can point it at a fake tree under tests/fixtures/sys/.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hwdiag::sysfs {

// root + an absolute path: under("/", "/proc/meminfo") == "/proc/meminfo",
// under("tests/fixtures/sys/desktop", "/proc/meminfo") == ".../proc/meminfo".
std::string under(const std::string& root, std::string_view path);

std::string trim(std::string_view s);

// Whole file, trimmed. nullopt if it can't be read.
std::optional<std::string> read_text(const std::string& path);
std::optional<long long> read_int(const std::string& path);
std::optional<double> read_double(const std::string& path);

// Entries of a directory whose names start with prefix, in natural order
// (cpu2 before cpu10). Empty if the directory is missing.
std::vector<std::string> list_dir(const std::string& dir, std::string_view prefix = "");

// "cpu12" with prefix "cpu" -> 12; nullopt if the rest is not all digits.
std::optional<int> numeric_suffix(std::string_view name, std::string_view prefix);

bool natural_less(const std::string& a, const std::string& b);

// "0-3,8,10-11" -> {0,1,2,3,8,10,11}.
std::vector<int> parse_cpu_list(std::string_view s);

// Sizes as sysfs and the CLIs write them: "48K", "2048K", "260M", "1G", or
// plain bytes. K/M/G are binary (KiB, MiB, GiB). nullopt if malformed.
std::optional<uint64_t> parse_size(std::string_view s);

// "16M" style label for a byte count, used in output and plots.
std::string format_size(uint64_t bytes);

}  // namespace hwdiag::sysfs
