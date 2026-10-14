#include "common/args.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "common/sysfs.hpp"

namespace hwdiag {

ArgParser::ArgParser(std::string prog, std::string description)
    : prog_(std::move(prog)), description_(std::move(description)) {}

void ArgParser::option(const std::string& name, const std::string& help, const std::string& default_value) {
  order_.push_back(name);
  specs_[name] = Spec{help, default_value, false};
}

void ArgParser::flag(const std::string& name, const std::string& help) {
  order_.push_back(name);
  specs_[name] = Spec{help, "", true};
}

bool ArgParser::parse(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  return parse(args);
}

bool ArgParser::parse(const std::vector<std::string>& args) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "-h" || arg == "--help") {
      help_ = true;
      continue;
    }
    if (arg.rfind("--", 0) != 0) {
      error_ = "unexpected argument '" + arg + "'";
      return false;
    }
    std::string name = arg.substr(2);
    std::string value;
    bool has_value = false;
    if (auto eq = name.find('='); eq != std::string::npos) {
      value = name.substr(eq + 1);
      name = name.substr(0, eq);
      has_value = true;
    }
    auto it = specs_.find(name);
    if (it == specs_.end()) {
      error_ = "unknown option --" + name;
      return false;
    }
    if (it->second.is_flag) {
      if (has_value) {
        error_ = "--" + name + " takes no value";
        return false;
      }
      values_[name] = "1";
      continue;
    }
    if (!has_value) {
      if (i + 1 >= args.size()) {
        error_ = "--" + name + " needs a value";
        return false;
      }
      value = args[++i];
    }
    values_[name] = value;
  }
  return true;
}

std::string ArgParser::usage() const {
  std::ostringstream ss;
  ss << "usage: " << prog_ << " [options]\n\n" << description_ << "\n\noptions:\n";
  for (const auto& name : order_) {
    const Spec& s = specs_.at(name);
    std::string left = "  --" + name + (s.is_flag ? "" : " VALUE");
    ss << left;
    if (left.size() < 26) ss << std::string(26 - left.size(), ' ');
    else ss << "\n" << std::string(26, ' ');
    ss << s.help;
    if (!s.is_flag && !s.default_value.empty()) ss << " (default: " << s.default_value << ")";
    ss << "\n";
  }
  ss << "  --help                  show this help\n";
  return ss.str();
}

bool ArgParser::given(const std::string& name) const { return values_.count(name) > 0; }

bool ArgParser::on(const std::string& flag_name) const { return values_.count(flag_name) > 0; }

std::string ArgParser::str(const std::string& name) const {
  if (auto it = values_.find(name); it != values_.end()) return it->second;
  if (auto it = specs_.find(name); it != specs_.end()) return it->second.default_value;
  throw std::invalid_argument("no option --" + name);
}

long long ArgParser::integer(const std::string& name) const {
  const std::string v = str(name);
  char* end = nullptr;
  const long long n = std::strtoll(v.c_str(), &end, 10);
  if (v.empty() || *end != '\0') throw std::invalid_argument("--" + name + ": not an integer: '" + v + "'");
  return n;
}

double ArgParser::number(const std::string& name) const {
  const std::string v = str(name);
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (v.empty() || *end != '\0') throw std::invalid_argument("--" + name + ": not a number: '" + v + "'");
  return d;
}

unsigned long long ArgParser::size(const std::string& name) const {
  const std::string v = str(name);
  auto s = sysfs::parse_size(v);
  if (!s) throw std::invalid_argument("--" + name + ": not a size (e.g. 64K, 16M, 1G): '" + v + "'");
  return *s;
}

}  // namespace hwdiag
