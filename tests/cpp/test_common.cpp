#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "common/affinity.hpp"
#include "common/args.hpp"
#include "common/json.hpp"
#include "common/sysfs.hpp"
#include "common/timing.hpp"

using namespace hwdiag;

TEST(Json, EscapesQuotesBackslashesAndControlCharacters) {
  EXPECT_EQ(json::quote("a\"b\\c"), R"("a\"b\\c")");
  EXPECT_EQ(json::quote("line\nnext\ttab"), R"("line\nnext\ttab")");
  EXPECT_EQ(json::quote(std::string("\x01\x1f", 2)), R"("\u0001\u001f")");
}

TEST(Json, KeepsValidUtf8AndReplacesInvalidBytes) {
  EXPECT_EQ(json::quote("45\xc2\xb0" "C"), "\"45\xc2\xb0" "C\"");
  EXPECT_EQ(json::quote("bad\xff"), "\"bad\\ufffd\"");
}

TEST(Json, NonFiniteNumbersBecomeNull) {
  auto v = json::Value::object();
  v["nan"] = std::numeric_limits<double>::quiet_NaN();
  v["inf"] = std::numeric_limits<double>::infinity();
  v["x"] = 1.5;
  EXPECT_EQ(v.dump(), R"({"nan":null,"inf":null,"x":1.5})");
}

TEST(Json, ObjectsKeepInsertionOrderAndNest) {
  auto v = json::Value::object();
  v["b"] = 1;
  v["a"] = "two";
  v["list"].push_back(true);
  v["list"].push_back(nullptr);
  v["b"] = 3;  // overwrite keeps position
  EXPECT_EQ(v.dump(), R"({"b":3,"a":"two","list":[true,null]})");
  EXPECT_EQ(json::Value::array().dump(), "[]");
  EXPECT_EQ(json::Value::object().dump(), "{}");
}

TEST(Json, PrettyPrintIndents) {
  auto v = json::Value::object();
  v["k"].push_back(1);
  EXPECT_EQ(v.dump(2), "{\n  \"k\": [\n    1\n  ]\n}");
}

TEST(Json, LargeUnsignedAndNegativeIntegers) {
  auto v = json::Value::array();
  v.push_back(18446744073709551615ull);
  v.push_back(-42LL);
  EXPECT_EQ(v.dump(), "[18446744073709551615,-42]");
}

TEST(Stats, MedianOfOddAndEvenCounts) {
  EXPECT_DOUBLE_EQ(median({3.0, 1.0, 2.0}), 2.0);
  EXPECT_DOUBLE_EQ(median({4.0, 1.0, 3.0, 2.0}), 2.5);
  EXPECT_TRUE(std::isnan(median({})));
}

TEST(Stats, SummaryReportsSpreadAroundTheMedian) {
  const Summary s = summarize({10.0, 12.0, 11.0, 9.0, 13.0});
  EXPECT_EQ(s.n, 5u);
  EXPECT_DOUBLE_EQ(s.median, 11.0);
  EXPECT_DOUBLE_EQ(s.min, 9.0);
  EXPECT_DOUBLE_EQ(s.max, 13.0);
  EXPECT_DOUBLE_EQ(s.mean, 11.0);
  EXPECT_NEAR(s.stdev, std::sqrt(2.5), 1e-12);
  EXPECT_NEAR(s.spread_pct, 4.0 / 11.0 * 100.0, 1e-12);
}

TEST(Stats, SingleSampleHasZeroStdevAndEmptyIsNan) {
  EXPECT_DOUBLE_EQ(summarize({5.0}).stdev, 0.0);
  EXPECT_TRUE(std::isnan(summarize({}).median));
}

TEST(Timing, MonotonicClockAdvances) {
  const uint64_t a = now_ns();
  const uint64_t b = now_ns();
  EXPECT_LE(a, b);
}

TEST(Sysfs, ParsesCacheAndCliSizes) {
  EXPECT_EQ(sysfs::parse_size("48K"), 48u * 1024);
  EXPECT_EQ(sysfs::parse_size("2048K\n"), 2048u * 1024);
  EXPECT_EQ(sysfs::parse_size("260M"), 260ull << 20);
  EXPECT_EQ(sysfs::parse_size("1G"), 1ull << 30);
  EXPECT_EQ(sysfs::parse_size("4096"), 4096u);
  EXPECT_EQ(sysfs::parse_size("16MiB"), 16ull << 20);
  EXPECT_FALSE(sysfs::parse_size("lots").has_value());
  EXPECT_FALSE(sysfs::parse_size("12Q").has_value());
}

TEST(Sysfs, FormatsSizes) {
  EXPECT_EQ(sysfs::format_size(4096), "4K");
  EXPECT_EQ(sysfs::format_size(6144), "6K");
  EXPECT_EQ(sysfs::format_size(3ull << 29), "1.5G");
  EXPECT_EQ(sysfs::format_size(100), "100B");
}

TEST(Sysfs, ParsesCpuLists) {
  EXPECT_EQ(sysfs::parse_cpu_list("0-3,8,10-11\n"), (std::vector<int>{0, 1, 2, 3, 8, 10, 11}));
  EXPECT_TRUE(sysfs::parse_cpu_list("").empty());
}

TEST(Sysfs, NaturalOrderPutsCpu2BeforeCpu10) {
  EXPECT_TRUE(sysfs::natural_less("cpu2", "cpu10"));
  EXPECT_FALSE(sysfs::natural_less("cpu10", "cpu2"));
  EXPECT_TRUE(sysfs::natural_less("temp1_input", "temp1_label"));
}

TEST(Sysfs, JoinsRootAndAbsolutePath) {
  EXPECT_EQ(sysfs::under("/", "/proc/meminfo"), "/proc/meminfo");
  EXPECT_EQ(sysfs::under("fake", "/proc/meminfo"), "fake/proc/meminfo");
}

TEST(Sysfs, NumericSuffix) {
  EXPECT_EQ(sysfs::numeric_suffix("cpu12", "cpu"), 12);
  EXPECT_FALSE(sysfs::numeric_suffix("cpufreq", "cpu").has_value());
  EXPECT_FALSE(sysfs::numeric_suffix("cpu", "cpu").has_value());
}

TEST(Affinity, SpreadOrderUsesEveryPhysicalCoreBeforeSmtSiblings) {
  // 2 cores x 2 threads, Intel-style numbering: cpu0/cpu2 share core 0.
  std::vector<CpuTopo> topo = {{0, 0, 0, 0}, {1, 0, 0, 1}, {2, 0, 0, 0}, {3, 0, 0, 1}};
  EXPECT_EQ(spread_order(topo), (std::vector<int>{0, 1, 2, 3}));
  // AMD-style numbering: cpu0/cpu1 share core 0.
  topo = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 1}, {3, 0, 0, 1}};
  EXPECT_EQ(spread_order(topo), (std::vector<int>{0, 2, 1, 3}));
}

TEST(Affinity, AllowedCpusIsNotEmpty) { EXPECT_FALSE(allowed_cpus().empty()); }

TEST(Args, ParsesOptionsFlagsAndDefaults) {
  ArgParser p("t", "test");
  p.option("size", "a size", "4K");
  p.option("reps", "reps", "7");
  p.flag("pretty", "a flag");
  ASSERT_TRUE(p.parse(std::vector<std::string>{"--size", "16M", "--pretty"}));
  EXPECT_EQ(p.size("size"), 16ull << 20);
  EXPECT_EQ(p.integer("reps"), 7);
  EXPECT_TRUE(p.on("pretty"));
  EXPECT_TRUE(p.given("size"));
  EXPECT_FALSE(p.given("reps"));
}

TEST(Args, AcceptsEqualsFormAndRejectsUnknownOptions) {
  ArgParser p("t", "test");
  p.option("reps", "reps", "7");
  ASSERT_TRUE(p.parse(std::vector<std::string>{"--reps=5"}));
  EXPECT_EQ(p.integer("reps"), 5);
  ArgParser q("t", "test");
  EXPECT_FALSE(q.parse(std::vector<std::string>{"--nope"}));
  EXPECT_NE(q.error().find("--nope"), std::string::npos);
}

TEST(Args, BadNumbersThrow) {
  ArgParser p("t", "test");
  p.option("reps", "reps", "x");
  ASSERT_TRUE(p.parse(std::vector<std::string>{}));
  EXPECT_THROW(p.integer("reps"), std::invalid_argument);
}
