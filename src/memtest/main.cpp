// memtest: a user-space memory test with fault injection.
#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>

#include "common/args.hpp"
#include "common/memory.hpp"
#include "common/result.hpp"
#include "common/sysfs.hpp"
#include "common/timing.hpp"
#include "memtest/patterns.hpp"

using namespace hwdiag;
using namespace hwdiag::memtest;

int main(int argc, char** argv) {
  ArgParser args("memtest",
                 "Writes and checks test patterns over a locked buffer and logs every flipped bit.\n"
                 "A user-space test only sees memory the kernel hands it; it demonstrates the\n"
                 "method and does not replace memtest86+.");
  args.option("size", "buffer size (default: 25% of MemAvailable)");
  args.option("patterns", "comma-separated patterns (default: all)");
  args.option("passes", "times to run the pattern list", "1");
  args.option("seed", "seed for the random pattern", "1");
  args.option("inject-fault", "flip one bit between writing and checking: <offset>:<bit>");
  args.option("max-errors", "errors to store in the result (all are counted)", "1000");
  args.option("out", "write JSON here instead of stdout", "-");
  args.flag("no-lock", "don't mlock the buffer");
  args.flag("quiet", "no progress lines on stderr");
  args.flag("compact", "write compact JSON");
  if (!args.parse(argc, argv)) {
    std::cerr << "memtest: " << args.error() << "\n\n" << args.usage();
    return kExitError;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return kExitOk;
  }

  try {
    std::vector<std::string> warnings;
    const uint64_t avail = mem_available_bytes();
    uint64_t size = args.given("size") ? args.size("size") : avail / 4;
    size = size / sizeof(uint64_t) * sizeof(uint64_t);
    if (size < 4096) throw std::invalid_argument("buffer too small (need at least 4K)");

    std::vector<const Pattern*> patterns;
    if (args.given("patterns")) {
      std::stringstream ss(args.str("patterns"));
      std::string name;
      while (std::getline(ss, name, ',')) {
        const Pattern* p = find_pattern(name);
        if (!p) throw std::invalid_argument("unknown pattern '" + name + "'");
        patterns.push_back(p);
      }
    } else {
      for (const auto& p : all_patterns()) patterns.push_back(&p);
    }
    const int passes = static_cast<int>(args.integer("passes"));
    if (passes < 1) throw std::invalid_argument("--passes must be at least 1");

    std::optional<Fault> fault;
    if (args.given("inject-fault")) {
      fault = parse_fault(args.str("inject-fault"));
      if (fault->offset >= size) throw std::invalid_argument("--inject-fault offset is past the end of the buffer");
    }

    MappedBuffer buf(size, Pages::Default);
    bool locked = false;
    if (!args.on("no-lock")) {
      if (mlock(buf.data(), size) == 0) {
        locked = true;
      } else {
        warnings.push_back(std::string("mlock failed (") + std::strerror(errno) +
                           "): raise ulimit -l (e.g. ulimit -l unlimited); testing unlocked memory, which may be swapped");
      }
    }

    ErrorLog log(static_cast<size_t>(args.integer("max-errors")));
    Context ctx;
    ctx.words = static_cast<uint64_t*>(buf.data());
    ctx.count = size / sizeof(uint64_t);
    ctx.log = &log;
    ctx.fault = fault;
    ctx.seed = static_cast<uint64_t>(args.integer("seed"));

    const bool quiet = args.on("quiet");
    if (!quiet) {
      std::cerr << "memtest: " << sysfs::format_size(size) << (locked ? " locked" : " unlocked") << ", "
                << patterns.size() << " pattern(s) x " << passes << " pass(es)\n";
    }
    auto runs = json::Value::array();
    const uint64_t t_all = now_ns();
    for (int pass = 1; pass <= passes; ++pass) {
      for (const Pattern* p : patterns) {
        const uint64_t before = log.total();
        ctx.injected = false;
        const uint64_t t0 = now_ns();
        p->run(ctx);
        const uint64_t t1 = now_ns();
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        const uint64_t errors = log.total() - before;
        auto r = json::Value::object();
        r["name"] = p->name;
        r["pass"] = pass;
        r["catches"] = p->catches;
        r["errors"] = errors;
        r["seconds"] = secs;
        r["gbs"] = static_cast<double>(size) * p->sweeps / (secs * 1e9);
        runs.push_back(std::move(r));
        if (!quiet) {
          std::cerr << "memtest: pass " << pass << " " << p->name << ": " << errors << " error(s), " << secs << " s\n";
        }
      }
    }
    const double total_secs = static_cast<double>(now_ns() - t_all) / 1e9;
    if (locked) munlock(buf.data(), size);

    auto errors = json::Value::array();
    for (const auto& e : log.stored()) {
      auto j = to_json(e);
      const auto phys = physical_address(static_cast<const char*>(buf.data()) + e.offset);
      j["physical_address"] = phys ? json::Value(*phys) : json::Value(nullptr);
      errors.push_back(std::move(j));
    }

    auto params = json::Value::object();
    params["size_bytes"] = size;
    params["mem_available_bytes"] = avail;
    params["passes"] = passes;
    params["seed"] = ctx.seed;
    auto names = json::Value::array();
    for (const Pattern* p : patterns) names.push_back(p->name);
    params["patterns"] = std::move(names);
    if (fault) {
      auto f = json::Value::object();
      f["offset"] = static_cast<uint64_t>(fault->offset);
      f["bit"] = fault->bit;
      params["inject_fault"] = std::move(f);
    } else {
      params["inject_fault"] = nullptr;
    }

    auto results = json::Value::object();
    results["bytes_tested"] = size;
    results["locked"] = locked;
    results["seconds"] = total_secs;
    results["patterns"] = std::move(runs);
    results["total_errors"] = log.total();
    results["errors"] = std::move(errors);
    results["errors_truncated"] = log.truncated();
    results["status"] = log.total() == 0 ? "pass" : "fail";
    if (log.total() > 0) {
      warnings.push_back(std::to_string(log.total()) + " memory error(s) found" +
                         (fault ? " (a fault was injected with --inject-fault)" : ""));
    }

    if (!write_result(make_result("memtest", params, results, warnings), args.str("out"), !args.on("compact"))) {
      return kExitError;
    }
    return log.total() == 0 ? kExitOk : kExitProblem;
  } catch (const std::exception& e) {
    std::cerr << "memtest: " << e.what() << "\n";
    return kExitError;
  }
}
