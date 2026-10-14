// The shared result format (format_version 1) that every tool writes.
#pragma once

#include <string>
#include <vector>

#include "common/json.hpp"

namespace hwdiag {

inline constexpr int kFormatVersion = 1;

// Exit codes shared by all tools.
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;     // bad usage or a runtime error
inline constexpr int kExitProblem = 2;   // ran fine, found a problem

// HWDIAG_GIT_COMMIT from the environment, else the commit baked in at
// configure time, else "unknown".
std::string git_commit();

// Current UTC time, ISO 8601 with a Z suffix.
std::string utc_timestamp();

json::Value make_result(const std::string& tool, json::Value params, json::Value results,
                        const std::vector<std::string>& warnings, const std::string& root = "/");

// Writes the JSON to path, or to stdout when path is empty or "-".
// Returns false (and prints why to stderr) if the file can't be written.
bool write_result(const json::Value& result, const std::string& path, bool pretty = true);

}  // namespace hwdiag
