#include <gtest/gtest.h>

#include <vector>

#include "memtest/patterns.hpp"

using namespace hwdiag::memtest;

namespace {

struct Buffer {
  std::vector<uint64_t> words;
  ErrorLog log;
  Context ctx;
  explicit Buffer(size_t count, std::optional<Fault> fault = std::nullopt) : words(count) {
    ctx.words = words.data();
    ctx.count = count;
    ctx.log = &log;
    ctx.fault = fault;
    ctx.seed = 1234;
  }
};

// What each pattern writes into word `index` in its first write phase, i.e.
// the value a fault injected right after that phase corrupts.
uint64_t first_value(const std::string& name, const Buffer& b, size_t index) {
  if (name == "zeros") return 0;
  if (name == "ones") return ~uint64_t{0};
  if (name == "checkerboard") return index % 2 == 0 ? kCheckerA : kCheckerB;
  if (name == "walking_ones") return uint64_t{1} << (index % 64);
  if (name == "walking_zeros") return ~(uint64_t{1} << (index % 64));
  if (name == "address_in_address") return reinterpret_cast<uint64_t>(&b.words[index]);
  if (name == "moving_inversions") return 0;
  if (name == "random") return random_word(1234, index);
  ADD_FAILURE() << "no expected value for " << name;
  return 0;
}

}  // namespace

class EveryPattern : public ::testing::TestWithParam<Pattern> {};

TEST_P(EveryPattern, FindsNothingWithoutAFault) {
  Buffer b(4096);
  GetParam().run(b.ctx);
  EXPECT_EQ(b.log.total(), 0u);
}

TEST_P(EveryPattern, CatchesTheInjectedFaultExactlyOnce) {
  const Pattern& p = GetParam();
  for (const Fault f : {Fault{8 * 123, 17}, Fault{0, 0}, Fault{8 * 4095, 63}}) {
    Buffer b(4096, f);
    p.run(b.ctx);
    ASSERT_EQ(b.log.total(), 1u) << p.name << " fault at " << f.offset << ":" << f.bit;
    const MemError& e = b.log.stored().front();
    EXPECT_EQ(e.pattern, p.name);
    EXPECT_EQ(e.offset, f.offset);
    EXPECT_EQ(e.xor_mask(), uint64_t{1} << f.bit);
    EXPECT_EQ(e.expected, first_value(p.name, b, f.offset / 8));
    EXPECT_EQ(e.actual, e.expected ^ (uint64_t{1} << f.bit));
  }
}

INSTANTIATE_TEST_SUITE_P(Patterns, EveryPattern, ::testing::ValuesIn(all_patterns()),
                         [](const ::testing::TestParamInfo<Pattern>& info) { return std::string(info.param.name); });

TEST(Memtest, HasEveryPatternFromTheSpec) {
  for (const char* name : {"zeros", "ones", "checkerboard", "walking_ones", "walking_zeros", "address_in_address",
                           "moving_inversions", "random"}) {
    EXPECT_NE(find_pattern(name), nullptr) << name;
  }
  EXPECT_EQ(find_pattern("nope"), nullptr);
}

TEST(Memtest, ParsesFaultOffsetsAndBits) {
  const Fault f = parse_fault("1024:3");
  EXPECT_EQ(f.offset, 1024u);
  EXPECT_EQ(f.bit, 3u);
  EXPECT_EQ(parse_fault("0x400:63").offset, 1024u);
  EXPECT_THROW(parse_fault("1023:3"), std::invalid_argument);  // not 8-byte aligned
  EXPECT_THROW(parse_fault("8:64"), std::invalid_argument);
  EXPECT_THROW(parse_fault("8"), std::invalid_argument);
  EXPECT_THROW(parse_fault("abc:1"), std::invalid_argument);
}

TEST(Memtest, ErrorLogStoresTheFirstErrorsAndCountsAll) {
  ErrorLog log(2);
  for (int i = 0; i < 5; ++i) log.add("zeros", static_cast<size_t>(i) * 8, 0, 1);
  EXPECT_EQ(log.total(), 5u);
  EXPECT_EQ(log.stored().size(), 2u);
  EXPECT_TRUE(log.truncated());
}

TEST(Memtest, ErrorJsonShowsWhichBitsFlipped) {
  const MemError e{"ones", 64, ~uint64_t{0}, ~uint64_t{0} ^ 0x81};
  const auto j = to_json(e).dump();
  EXPECT_NE(j.find("\"xor_mask\":\"0x0000000000000081\""), std::string::npos) << j;
  EXPECT_NE(j.find("\"flipped_bits\":[0,7]"), std::string::npos) << j;
  EXPECT_NE(j.find("\"offset_hex\":\"0x0000000000000040\""), std::string::npos) << j;
}
