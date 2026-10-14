// A tiny command-line parser: --name value, --name=value and --flag.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace hwdiag {

class ArgParser {
 public:
  ArgParser(std::string prog, std::string description);

  void option(const std::string& name, const std::string& help, const std::string& default_value = "");
  void flag(const std::string& name, const std::string& help);

  // False on a parse error (see error()). --help sets help_requested().
  bool parse(int argc, char** argv);
  bool parse(const std::vector<std::string>& args);

  bool help_requested() const { return help_; }
  const std::string& error() const { return error_; }
  std::string usage() const;

  bool given(const std::string& name) const;  // set on the command line
  bool on(const std::string& flag_name) const;
  std::string str(const std::string& name) const;
  // These throw std::invalid_argument with the option name on bad input.
  long long integer(const std::string& name) const;
  double number(const std::string& name) const;
  // A size with an optional K/M/G suffix (binary units).
  unsigned long long size(const std::string& name) const;

 private:
  struct Spec {
    std::string help;
    std::string default_value;
    bool is_flag = false;
  };
  std::string prog_;
  std::string description_;
  std::vector<std::string> order_;
  std::map<std::string, Spec> specs_;
  std::map<std::string, std::string> values_;
  bool help_ = false;
  std::string error_;
};

}  // namespace hwdiag
