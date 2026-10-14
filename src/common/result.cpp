#include "common/result.hpp"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>

#include "build_info.hpp"
#include "common/hostinfo.hpp"

namespace hwdiag {

std::string git_commit() {
  if (const char* env = std::getenv("HWDIAG_GIT_COMMIT"); env && *env) return env;
  const std::string baked = build::kGitCommit;
  return baked.empty() ? "unknown" : baked;
}

std::string utc_timestamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

json::Value make_result(const std::string& tool, json::Value params, json::Value results,
                        const std::vector<std::string>& warnings, const std::string& root) {
  auto r = json::Value::object();
  r["format_version"] = kFormatVersion;
  r["tool"] = tool;
  r["tool_version"] = build::kVersion;
  r["timestamp"] = utc_timestamp();
  r["git_commit"] = git_commit();
  r["host"] = to_json(collect_host_info(root));
  r["params"] = params.is_null() ? json::Value::object() : std::move(params);
  r["results"] = results.is_null() ? json::Value::object() : std::move(results);
  auto w = json::Value::array();
  for (const auto& s : warnings) w.push_back(s);
  r["warnings"] = std::move(w);
  return r;
}

bool write_result(const json::Value& result, const std::string& path, bool pretty) {
  const std::string text = result.dump(pretty ? 2 : -1) + "\n";
  if (path.empty() || path == "-") {
    std::cout << text << std::flush;
    return static_cast<bool>(std::cout);
  }
  std::ofstream out(path);
  if (!out) {
    std::cerr << "error: cannot write " << path << "\n";
    return false;
  }
  out << text;
  out.close();
  if (!out) {
    std::cerr << "error: failed writing " << path << "\n";
    return false;
  }
  return true;
}

}  // namespace hwdiag
