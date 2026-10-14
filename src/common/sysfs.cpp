#include "common/sysfs.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace hwdiag::sysfs {

std::string under(const std::string& root, std::string_view path) {
  while (!path.empty() && path.front() == '/') path.remove_prefix(1);
  if (root.empty() || root == "/") return "/" + std::string(path);
  return (fs::path(root) / fs::path(path)).string();
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::optional<std::string> read_text(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return trim(ss.str());
}

std::optional<long long> read_int(const std::string& path) {
  auto text = read_text(path);
  if (!text || text->empty()) return std::nullopt;
  // Base 16 only with an explicit 0x prefix (PCI ids); never octal.
  const bool hex = text->size() > 2 && (*text)[0] == '0' && ((*text)[1] == 'x' || (*text)[1] == 'X');
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(text->c_str(), &end, hex ? 16 : 10);
  if (errno != 0 || end == text->c_str()) return std::nullopt;
  return v;
}

std::optional<double> read_double(const std::string& path) {
  auto text = read_text(path);
  if (!text || text->empty()) return std::nullopt;
  char* end = nullptr;
  double v = std::strtod(text->c_str(), &end);
  if (end == text->c_str()) return std::nullopt;
  return v;
}

bool natural_less(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const bool da = std::isdigit(static_cast<unsigned char>(a[i]));
    const bool db = std::isdigit(static_cast<unsigned char>(b[j]));
    if (da && db) {
      size_t ie = i, je = j;
      while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
      while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
      // Compare digit runs as numbers without converting (no overflow):
      // strip leading zeros, then a longer run is bigger.
      size_t ia = i, jb = j;
      while (ia + 1 < ie && a[ia] == '0') ++ia;
      while (jb + 1 < je && b[jb] == '0') ++jb;
      if (ie - ia != je - jb) return ie - ia < je - jb;
      const int cmp = a.compare(ia, ie - ia, b, jb, je - jb);
      if (cmp != 0) return cmp < 0;
      i = ie;
      j = je;
    } else {
      if (a[i] != b[j]) return a[i] < b[j];
      ++i;
      ++j;
    }
  }
  return a.size() - i < b.size() - j;
}

std::vector<std::string> list_dir(const std::string& dir, std::string_view prefix) {
  std::vector<std::string> names;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return names;
  for (const auto& entry : it) {
    std::string name = entry.path().filename().string();
    if (name.compare(0, prefix.size(), prefix) == 0) names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end(), natural_less);
  return names;
}

std::optional<int> numeric_suffix(std::string_view name, std::string_view prefix) {
  if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix) return std::nullopt;
  std::string_view rest = name.substr(prefix.size());
  if (rest.size() > 9) return std::nullopt;
  int v = 0;
  for (char c : rest) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    v = v * 10 + (c - '0');
  }
  return v;
}

std::vector<int> parse_cpu_list(std::string_view s) {
  std::vector<int> cpus;
  std::string text = trim(s);
  std::stringstream ss(text);
  std::string part;
  while (std::getline(ss, part, ',')) {
    part = trim(part);
    if (part.empty()) continue;
    const auto dash = part.find('-');
    try {
      if (dash == std::string::npos) {
        cpus.push_back(std::stoi(part));
      } else {
        const int lo = std::stoi(part.substr(0, dash));
        const int hi = std::stoi(part.substr(dash + 1));
        for (int c = lo; c <= hi; ++c) cpus.push_back(c);
      }
    } catch (const std::exception&) {
      // Ignore malformed ranges rather than failing a whole inventory.
    }
  }
  return cpus;
}

std::optional<uint64_t> parse_size(std::string_view s) {
  std::string t = trim(s);
  if (t.empty()) return std::nullopt;
  size_t pos = 0;
  while (pos < t.size() && std::isdigit(static_cast<unsigned char>(t[pos]))) ++pos;
  if (pos == 0 || pos > 15) return std::nullopt;
  const uint64_t n = std::stoull(t.substr(0, pos));
  std::string suffix = trim(t.substr(pos));
  for (auto& c : suffix) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (suffix == "KIB" || suffix == "KB") suffix = "K";
  if (suffix == "MIB" || suffix == "MB") suffix = "M";
  if (suffix == "GIB" || suffix == "GB") suffix = "G";
  if (suffix.empty() || suffix == "B") return n;
  if (suffix == "K") return n << 10;
  if (suffix == "M") return n << 20;
  if (suffix == "G") return n << 30;
  return std::nullopt;
}

std::string format_size(uint64_t bytes) {
  const char* units[] = {"B", "K", "M", "G", "T"};
  int u = 0;
  double v = static_cast<double>(bytes);
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  std::ostringstream ss;
  if (v == static_cast<double>(static_cast<uint64_t>(v))) {
    ss << static_cast<uint64_t>(v) << units[u];
  } else {
    ss.precision(3);
    ss << v << units[u];
  }
  return ss.str();
}

}  // namespace hwdiag::sysfs
