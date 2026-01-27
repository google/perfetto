#include "perfetto/ext/base/regex.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(RegexTest, Match) {
  {
    Regex re("abc");
    EXPECT_TRUE(re.IsValid());
    EXPECT_TRUE(re.Match("abc"));
    EXPECT_FALSE(re.Match("abcd"));
    EXPECT_FALSE(re.Match("ab"));
    EXPECT_FALSE(re.Match("ABC"));
  }
  {
    Regex re("a.*c");
    EXPECT_TRUE(re.IsValid());
    EXPECT_TRUE(re.Match("abc"));
    EXPECT_TRUE(re.Match("abbbc"));
    EXPECT_TRUE(re.Match("ac"));
  }
}

TEST(RegexTest, Search) {
  Regex re("abc");
  EXPECT_TRUE(re.IsValid());
  EXPECT_TRUE(re.Search("abc"));
  EXPECT_TRUE(re.Search("xabcy"));
  EXPECT_TRUE(re.Search("abcabc"));
  EXPECT_FALSE(re.Search("abx"));
}

TEST(RegexTest, CaseInsensitive) {
  Regex re("abc", Regex::Option::kCaseInsensitive);
  EXPECT_TRUE(re.IsValid());
  EXPECT_TRUE(re.Match("abc"));
  EXPECT_TRUE(re.Match("ABC"));
  EXPECT_TRUE(re.Match("aBc"));
  EXPECT_TRUE(re.Search("xAbCy"));
}

TEST(RegexTest, Invalid) {
  Regex re("[a-z");
  EXPECT_FALSE(re.IsValid());
  EXPECT_FALSE(re.Match("a"));
}

TEST(RegexTest, Move) {
  Regex re1("abc");
  Regex re2 = std::move(re1);
  EXPECT_TRUE(re2.IsValid());
  EXPECT_TRUE(re2.Match("abc"));
}

TEST(RegexTest, Extract) {
  Regex re("a(b+)c");
  std::vector<std::string> matches;
  EXPECT_TRUE(re.Extract("abbc", matches));
  EXPECT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0], "abbc");
  EXPECT_EQ(matches[1], "bb");

  EXPECT_FALSE(re.Extract("axc", matches));
}

TEST(RegexTest, Create) {
  auto re_or = Regex::Create("abc");
  ASSERT_TRUE(re_or.ok());
  EXPECT_TRUE(re_or->Match("abc"));

  auto re_invalid = Regex::Create("[a-z");
  EXPECT_FALSE(re_invalid.ok());
}

TEST(RegexTest, Submatch) {
  Regex re("a(b)c(d)e");
  std::vector<std::string_view> matches;
  re.Submatch("abcde", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("abcde", "b", "d"));
}

TEST(RegexTest, SubmatchNoMatch) {
  Regex re("a(b)c(d)e");
  std::vector<std::string_view> matches;
  re.Submatch("fghij", matches);
  ASSERT_TRUE(matches.empty());
}

TEST(RegexTest, SubmatchOptionalGroup) {
  Regex re("a(b)?c");
  std::vector<std::string_view> matches;
  re.Submatch("ac", matches);
  ASSERT_FALSE(matches.empty());
  EXPECT_THAT(matches, testing::ElementsAre("ac", ""));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
