/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/ext/base/string_utils.h"

#include <optional>
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

template <size_t N>
struct UninitializedBuf {
  UninitializedBuf() { memset(data, '?', sizeof(data)); }
  operator char*() { return data; }
  char data[N];
};

using testing::ElementsAre;

TEST(StringUtilsTest, Lowercase) {
  EXPECT_EQ(Lowercase('A'), 'a');
  EXPECT_EQ(Lowercase('a'), 'a');
  EXPECT_EQ(Lowercase('Z'), 'z');
  EXPECT_EQ(Lowercase('z'), 'z');
  EXPECT_EQ(Lowercase('!'), '!');
}

TEST(StringUtilsTest, Uppercase) {
  EXPECT_EQ(Uppercase('A'), 'A');
  EXPECT_EQ(Uppercase('a'), 'A');
  EXPECT_EQ(Uppercase('Z'), 'Z');
  EXPECT_EQ(Uppercase('z'), 'Z');
  EXPECT_EQ(Uppercase('!'), '!');
}

TEST(StringUtilsTest, CStringToUInt32) {
  EXPECT_EQ(CStringToUInt32("0"), std::make_optional<uint32_t>(0U));
  EXPECT_EQ(CStringToUInt32("1"), std::make_optional<uint32_t>(1U));
  EXPECT_EQ(CStringToUInt32("42"), std::make_optional<uint32_t>(42U));
  EXPECT_EQ(CStringToUInt32(""), std::nullopt);
  EXPECT_EQ(CStringToUInt32("!?"), std::nullopt);
  EXPECT_EQ(CStringToUInt32("abc"), std::nullopt);
  EXPECT_EQ(CStringToUInt32("123 abc"), std::nullopt);
}

TEST(StringUtilsTest, CStringToInt32) {
  EXPECT_EQ(CStringToInt32("0"), std::make_optional<int32_t>(0));
  EXPECT_EQ(CStringToInt32("1"), std::make_optional<int32_t>(1));
  EXPECT_EQ(CStringToInt32("-42"), std::make_optional<int32_t>(-42));
  EXPECT_EQ(CStringToInt32(""), std::nullopt);
  EXPECT_EQ(CStringToInt32("!?"), std::nullopt);
  EXPECT_EQ(CStringToInt32("abc"), std::nullopt);
  EXPECT_EQ(CStringToInt32("123 abc"), std::nullopt);
}

TEST(StringUtilsTest, CStringToDouble) {
  EXPECT_DOUBLE_EQ(CStringToDouble("0").value(), 0l);
  EXPECT_DOUBLE_EQ(CStringToDouble("1").value(), 1l);
  EXPECT_DOUBLE_EQ(CStringToDouble("-42").value(), -42l);
  EXPECT_DOUBLE_EQ(CStringToDouble("-42.5").value(), -42.5l);
  EXPECT_EQ(CStringToDouble(""), std::nullopt);
  EXPECT_EQ(CStringToDouble("!?"), std::nullopt);
  EXPECT_EQ(CStringToDouble("abc"), std::nullopt);
  EXPECT_EQ(CStringToDouble("123 abc"), std::nullopt);
}

TEST(StringUtilsTest, StringToUInt32) {
  EXPECT_EQ(StringToUInt32("0"), std::make_optional<uint32_t>(0U));
  EXPECT_EQ(StringToUInt32("1"), std::make_optional<uint32_t>(1U));
  EXPECT_EQ(StringToUInt32("42"), std::make_optional<uint32_t>(42U));
  EXPECT_EQ(StringToUInt32("a", 16), std::make_optional<uint32_t>(10U));
  EXPECT_EQ(StringToUInt32("fffffff0", 16),
            std::make_optional<uint32_t>(0xfffffff0));
  EXPECT_EQ(StringToUInt32(""), std::nullopt);
  EXPECT_EQ(StringToUInt32("!?"), std::nullopt);
  EXPECT_EQ(StringToUInt32("abc"), std::nullopt);
  EXPECT_EQ(StringToUInt32("123 abc"), std::nullopt);
  EXPECT_EQ(StringToUInt32("beefz", 16), std::nullopt);
}

TEST(StringUtilsTest, StringToInt32) {
  EXPECT_EQ(StringToInt32("0"), std::make_optional<int32_t>(0));
  EXPECT_EQ(StringToInt32("1"), std::make_optional<int32_t>(1));
  EXPECT_EQ(StringToInt32("+42"), std::make_optional<int32_t>(42));
  EXPECT_EQ(StringToInt32("+0042"), std::make_optional<int32_t>(42));
  EXPECT_EQ(StringToInt32("-42"), std::make_optional<int32_t>(-42));
  EXPECT_EQ(StringToInt32("42", 16), std::make_optional<int32_t>(0x42));
  EXPECT_EQ(StringToInt32("7ffffffe", 16),
            std::make_optional<int32_t>(0x7ffffffe));
  EXPECT_EQ(StringToInt32(""), std::nullopt);
  EXPECT_EQ(StringToInt32("!?"), std::nullopt);
  EXPECT_EQ(StringToInt32("abc"), std::nullopt);
  EXPECT_EQ(StringToInt32("123 abc"), std::nullopt);
  EXPECT_EQ(StringToInt32("beefz", 16), std::nullopt);
}

TEST(StringUtilsTest, StringToUInt64) {
  EXPECT_EQ(StringToUInt64("0"), std::make_optional<uint64_t>(0u));
  EXPECT_EQ(StringToUInt64("1"), std::make_optional<uint64_t>(1u));
  EXPECT_EQ(StringToUInt64("5000000000"),
            std::make_optional<uint64_t>(5000000000ULL));
  EXPECT_EQ(StringToUInt64("7ffffffffffffffe", 16),
            std::make_optional<uint64_t>(0x7ffffffffffffffeULL));
  EXPECT_EQ(StringToUInt64("9ffffffffffffffe", 16),
            std::make_optional<uint64_t>(0x9ffffffffffffffeULL));
  EXPECT_EQ(StringToUInt64(""), std::nullopt);
  EXPECT_EQ(StringToUInt64("abc"), std::nullopt);
  EXPECT_EQ(StringToUInt64("beefz", 16), std::nullopt);
}

TEST(StringUtilsTest, StringToInt64) {
  EXPECT_EQ(StringToInt64("0"), std::make_optional<int64_t>(0));
  EXPECT_EQ(StringToInt64("1"), std::make_optional<int64_t>(1));
  EXPECT_EQ(StringToInt64("-5000000000"),
            std::make_optional<int64_t>(-5000000000LL));
  EXPECT_EQ(StringToInt64("5000000000"),
            std::make_optional<int64_t>(5000000000LL));
  EXPECT_EQ(StringToInt64("7ffffffffffffffe", 16),
            std::make_optional<int64_t>(0x7ffffffffffffffeLL));
  EXPECT_EQ(StringToInt64("9ffffffe", 16),
            std::make_optional<int64_t>(0x9ffffffeLL));
  EXPECT_EQ(StringToInt64(""), std::nullopt);
  EXPECT_EQ(StringToInt64("abc"), std::nullopt);
  EXPECT_EQ(StringToInt64("beefz", 16), std::nullopt);
}

TEST(StringUtilsTest, StringToDouble) {
  EXPECT_DOUBLE_EQ(StringToDouble("0").value(), 0l);
  EXPECT_DOUBLE_EQ(StringToDouble("1").value(), 1l);
  EXPECT_DOUBLE_EQ(StringToDouble("-42").value(), -42l);
  EXPECT_DOUBLE_EQ(StringToDouble("-42.5").value(), -42.5l);
  EXPECT_DOUBLE_EQ(StringToDouble("0.5").value(), .5l);
  EXPECT_DOUBLE_EQ(StringToDouble(".5").value(), .5l);
  EXPECT_EQ(StringToDouble(""), std::nullopt);
  EXPECT_EQ(StringToDouble("!?"), std::nullopt);
  EXPECT_EQ(StringToDouble("abc"), std::nullopt);
  EXPECT_EQ(StringToDouble("123 abc"), std::nullopt);
  EXPECT_EQ(StringToDouble("124,456"), std::nullopt);
  EXPECT_EQ(StringToDouble("4 2"), std::nullopt);
  EXPECT_EQ(StringToDouble(" - 42"), std::nullopt);
}

TEST(StringUtilsTest, StartsWith) {
  EXPECT_TRUE(StartsWith("", ""));
  EXPECT_TRUE(StartsWith("abc", ""));
  EXPECT_TRUE(StartsWith("abc", "a"));
  EXPECT_TRUE(StartsWith("abc", "ab"));
  EXPECT_TRUE(StartsWith("abc", "abc"));
  EXPECT_FALSE(StartsWith("abc", "abcd"));
  EXPECT_FALSE(StartsWith("aa", "ab"));
  EXPECT_FALSE(StartsWith("", "ab"));
}

TEST(StringUtilsTest, StartsWithAny) {
  EXPECT_FALSE(StartsWithAny("", {"a", "b"}));
  EXPECT_FALSE(StartsWithAny("abcd", {}));
  EXPECT_FALSE(StartsWithAny("", {}));
  EXPECT_TRUE(StartsWithAny("abcd", {"ac", "ab"}));
  EXPECT_FALSE(StartsWithAny("abcd", {"bc", "ac"}));
}

TEST(StringUtilsTest, EndsWith) {
  EXPECT_TRUE(EndsWith("", ""));
  EXPECT_TRUE(EndsWith("abc", ""));
  EXPECT_TRUE(EndsWith("abc", "c"));
  EXPECT_TRUE(EndsWith("abc", "bc"));
  EXPECT_TRUE(EndsWith("abc", "abc"));
  EXPECT_FALSE(EndsWith("bcd", "abcd"));
  EXPECT_FALSE(EndsWith("abc", "abd"));
  EXPECT_FALSE(EndsWith("", "c"));
}

TEST(StringUtilsTest, ToHex) {
  EXPECT_EQ(ToHex(""), "");
  EXPECT_EQ(ToHex("abc123"), "616263313233");
}

TEST(StringUtilsTest, IntToHex) {
  EXPECT_EQ(IntToHexString(0), "0x00");
  EXPECT_EQ(IntToHexString(1), "0x01");
  EXPECT_EQ(IntToHexString(16), "0x10");
  EXPECT_EQ(IntToHexString(4294967295), "0xffffffff");
}

TEST(StringUtilsTest, Uint64ToHex) {
  EXPECT_EQ(Uint64ToHexString(0), "0x0");
  EXPECT_EQ(Uint64ToHexString(1), "0x1");
  EXPECT_EQ(Uint64ToHexString(16), "0x10");
  EXPECT_EQ(Uint64ToHexString(18446744073709551615UL), "0xffffffffffffffff");
}

TEST(StringUtilsTest, Uint64ToHexNoPrefix) {
  EXPECT_EQ(Uint64ToHexStringNoPrefix(0), "0");
  EXPECT_EQ(Uint64ToHexStringNoPrefix(1), "1");
  EXPECT_EQ(Uint64ToHexStringNoPrefix(16), "10");
  EXPECT_EQ(Uint64ToHexStringNoPrefix(18446744073709551615UL),
            "ffffffffffffffff");
}

TEST(StringUtilsTest, CaseInsensitiveEqual) {
  EXPECT_TRUE(CaseInsensitiveEqual("", ""));
  EXPECT_TRUE(CaseInsensitiveEqual("abc", "abc"));
  EXPECT_TRUE(CaseInsensitiveEqual("ABC", "abc"));
  EXPECT_TRUE(CaseInsensitiveEqual("abc", "ABC"));
  EXPECT_FALSE(CaseInsensitiveEqual("abc", "AB"));
  EXPECT_FALSE(CaseInsensitiveEqual("ab", "ABC"));
}

TEST(StringUtilsTest, SplitString) {
  EXPECT_THAT(SplitString("", ":"), ElementsAre());
  EXPECT_THAT(SplitString("a:b:c", ":"), ElementsAre("a", "b", "c"));
  EXPECT_THAT(SplitString("a::b::c", "::"), ElementsAre("a", "b", "c"));
  EXPECT_THAT(SplitString("::::a::b::::c::", "::"), ElementsAre("a", "b", "c"));
  EXPECT_THAT(SplitString("abc", ":"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", "::"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", ":"), ElementsAre("abc"));
  EXPECT_THAT(SplitString("abc", "::"), ElementsAre("abc"));
}

TEST(StringUtilsTest, Strip) {
  EXPECT_EQ(StripPrefix("abc", ""), "abc");
  EXPECT_EQ(StripPrefix("abc", "a"), "bc");
  EXPECT_EQ(StripPrefix("abc", "ab"), "c");
  EXPECT_EQ(StripPrefix("abc", "abc"), "");
  EXPECT_EQ(StripPrefix("abc", "abcd"), "abc");

  EXPECT_EQ(StripSuffix("abc", ""), "abc");
  EXPECT_EQ(StripSuffix("abc", "c"), "ab");
  EXPECT_EQ(StripSuffix("abc", "bc"), "a");
  EXPECT_EQ(StripSuffix("abc", "abc"), "");
  EXPECT_EQ(StripSuffix("abc", "ebcd"), "abc");

  EXPECT_EQ(StripChars("foobar", "", '_'), "foobar");
  EXPECT_EQ(StripChars("foobar", "x", '_'), "foobar");
  EXPECT_EQ(StripChars("foobar", "f", '_'), "_oobar");
  EXPECT_EQ(StripChars("foobar", "o", '_'), "f__bar");
  EXPECT_EQ(StripChars("foobar", "oa", '_'), "f__b_r");
  EXPECT_EQ(StripChars("foobar", "fbr", '_'), "_oo_a_");
  EXPECT_EQ(StripChars("foobar", "froab", '_'), "______");
}

TEST(StringUtilsTest, TrimWhitespace) {
  EXPECT_EQ(TrimWhitespace(""), "");
  EXPECT_EQ(TrimWhitespace(" "), "");
  EXPECT_EQ(TrimWhitespace("\t\n"), "");

  EXPECT_EQ(TrimWhitespace("\tx\n\n"), "x");
  EXPECT_EQ(TrimWhitespace("\tx\n"), "x");
  EXPECT_EQ(TrimWhitespace("\tx\nx\n"), "x\nx");
}

TEST(StringUtilsTest, Contains) {
  EXPECT_TRUE(Contains("", ""));
  EXPECT_TRUE(Contains("abc", ""));
  EXPECT_TRUE(Contains("abc", "a"));
  EXPECT_TRUE(Contains("abc", "b"));
  EXPECT_TRUE(Contains("abc", "c"));
  EXPECT_TRUE(Contains("abc", "ab"));
  EXPECT_TRUE(Contains("abc", "bc"));
  EXPECT_TRUE(Contains("abc", "abc"));
  EXPECT_FALSE(Contains("abc", "d"));
  EXPECT_FALSE(Contains("abc", "ac"));
  EXPECT_FALSE(Contains("abc", "abcd"));
  EXPECT_FALSE(Contains("", "a"));
  EXPECT_FALSE(Contains("", "abc"));
}

TEST(StringUtilsTest, Find) {
  EXPECT_EQ(Find("", ""), 0u);
  EXPECT_EQ(Find("", "abc"), 0u);
  EXPECT_EQ(Find("a", "abc"), 0u);
  EXPECT_EQ(Find("b", "abc"), 1u);
  EXPECT_EQ(Find("c", "abc"), 2u);
  EXPECT_EQ(Find("ab", "abc"), 0u);
  EXPECT_EQ(Find("bc", "abc"), 1u);
  EXPECT_EQ(Find("abc", "abc"), 0u);
  EXPECT_EQ(Find("d", "abc"), std::string::npos);
  EXPECT_EQ(Find("ac", "abc"), std::string::npos);
  EXPECT_EQ(Find("abcd", "abc"), std::string::npos);
  EXPECT_EQ(Find("a", ""), std::string::npos);
  EXPECT_EQ(Find("abc", ""), std::string::npos);
}

TEST(StringUtilsTest, ReplaceAll) {
  EXPECT_EQ(ReplaceAll("", "a", ""), "");
  EXPECT_EQ(ReplaceAll("", "a", "b"), "");
  EXPECT_EQ(ReplaceAll("a", "a", "b"), "b");
  EXPECT_EQ(ReplaceAll("aaaa", "a", "b"), "bbbb");
  EXPECT_EQ(ReplaceAll("aaaa", "aa", "b"), "bb");
  EXPECT_EQ(ReplaceAll("aa", "aa", "bb"), "bb");
  EXPECT_EQ(ReplaceAll("aa", "a", "bb"), "bbbb");
  EXPECT_EQ(ReplaceAll("abc", "a", "b"), "bbc");
  EXPECT_EQ(ReplaceAll("abc", "c", "b"), "abb");
  EXPECT_EQ(ReplaceAll("abc", "c", "bbb"), "abbbb");
}

TEST(StringUtilsTest, StringCopy) {
  // Nothing should be written when |dst_size| = 0.
  {
    char dst[2] = {42, 43};
    StringCopy(dst, "12345", 0);
    EXPECT_EQ(42, dst[0]);
    EXPECT_EQ(43, dst[1]);
  }

  // Nominal case, len(src) < sizeof(dst).
  {
    UninitializedBuf<10> dst;
    StringCopy(dst, "1234567", sizeof(dst));
    EXPECT_STREQ(dst, "1234567");
  }

  // Edge case where we perfectly fit including the \0.
  {
    UninitializedBuf<8> dst;
    StringCopy(dst, "1234567", sizeof(dst));
    EXPECT_STREQ(dst, "1234567");
  }

  // Edge case where |dst| is smaller by one char.
  {
    UninitializedBuf<8> dst;
    StringCopy(dst, "12345678", sizeof(dst));
    EXPECT_STREQ(dst, "1234567");
  }

  // Case when |dst| is smaller than |src|.
  {
    UninitializedBuf<3> dst;
    StringCopy(dst, "12345678", sizeof(dst));
    EXPECT_STREQ(dst, "12");
  }
}

TEST(StringUtilsTest, SprintfTrunc) {
  {
    UninitializedBuf<3> dst;
    ASSERT_EQ(0u, SprintfTrunc(dst, sizeof(dst), "%s", ""));
    EXPECT_STREQ(dst, "");
  }

  {
    char dst[3]{'O', 'K', '\0'};
    ASSERT_EQ(0u, SprintfTrunc(dst, 0, "whatever"));
    EXPECT_STREQ(dst, "OK");  // dst_size == 0 shouldn't touch the buffer.
  }

  {
    UninitializedBuf<1> dst;
    ASSERT_EQ(0u, SprintfTrunc(dst, sizeof(dst), "whatever"));
    EXPECT_STREQ(dst, "");
  }

  {
    UninitializedBuf<3> dst;
    ASSERT_EQ(1u, SprintfTrunc(dst, sizeof(dst), "1"));
    EXPECT_STREQ(dst, "1");
  }

  {
    UninitializedBuf<3> dst;
    ASSERT_EQ(2u, SprintfTrunc(dst, sizeof(dst), "12"));
    EXPECT_STREQ(dst, "12");
  }

  {
    UninitializedBuf<3> dst;
    ASSERT_EQ(2u, SprintfTrunc(dst, sizeof(dst), "123"));
    EXPECT_STREQ(dst, "12");
  }

  {
    UninitializedBuf<3> dst;
    ASSERT_EQ(2u, SprintfTrunc(dst, sizeof(dst), "1234"));
    EXPECT_STREQ(dst, "12");
  }

  {
    UninitializedBuf<11> dst;
    ASSERT_EQ(10u, SprintfTrunc(dst, sizeof(dst), "a %d b %s", 42, "foo"));
    EXPECT_STREQ(dst, "a 42 b foo");
  }
}

TEST(StringUtilsTest, StackString) {
  {
    StackString<1> s("123");
    EXPECT_EQ(0u, s.len());
    EXPECT_STREQ("", s.c_str());
  }

  {
    StackString<4> s("123");
    EXPECT_EQ(3u, s.len());
    EXPECT_STREQ("123", s.c_str());
    EXPECT_EQ(s.ToStdString(), std::string(s.c_str()));
    EXPECT_EQ(s.string_view().ToStdString(), s.ToStdString());
  }

  {
    StackString<3> s("123");
    EXPECT_EQ(2u, s.len());
    EXPECT_STREQ("12", s.c_str());
    EXPECT_EQ(s.ToStdString(), std::string(s.c_str()));
    EXPECT_EQ(s.string_view().ToStdString(), s.ToStdString());
  }

  {
    StackString<11> s("foo %d %s", 42, "bar!!!OVERFLOW");
    EXPECT_EQ(10u, s.len());
    EXPECT_STREQ("foo 42 bar", s.c_str());
    EXPECT_EQ(s.ToStdString(), std::string(s.c_str()));
    EXPECT_EQ(s.string_view().ToStdString(), s.ToStdString());
  }
}

TEST(FindLineTest, InvalidOffset1) {
  std::string str = "abc\ndef\n\nghi";
  uint32_t offset = 3;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_FALSE(error.has_value());
}

TEST(FindLineTest, InvalidOffset2) {
  std::string str = "abc\ndef\n\nghi";
  uint32_t offset = 8;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_FALSE(error.has_value());
}

TEST(FindLineTest, FirstCharacter) {
  std::string str = "abc\ndef\n\nghi";
  uint32_t offset = 0;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_TRUE(error.has_value());
  ASSERT_EQ(error.value().line_num, 1ul);
  ASSERT_EQ(error.value().line_offset, 0ul);
  ASSERT_EQ(error.value().line, "abc");
}

TEST(FindLineTest, StandardCheck) {
  std::string str = "abc\ndef\n\nghi";
  uint32_t offset = 5;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_TRUE(error.has_value());
  ASSERT_EQ(error.value().line_num, 2ul);
  ASSERT_EQ(error.value().line_offset, 1ul);
  ASSERT_EQ(error.value().line, "def");
}

TEST(FindLineTest, TwoBreakLines) {
  std::string str = "abc\ndef\n\nghi";
  uint32_t offset = 10;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_TRUE(error.has_value());
  ASSERT_EQ(error.value().line_num, 4ul);
  ASSERT_EQ(error.value().line_offset, 1ul);
  ASSERT_EQ(error.value().line, "ghi");
}

TEST(FindLineTest, EndsWithBreakLine) {
  std::string str = "abc\ndef\n\nghi\n";
  uint32_t offset = 10;

  auto error = FindLineWithOffset(base::StringView(str), offset);

  EXPECT_TRUE(error.has_value());
  ASSERT_EQ(error.value().line_num, 4ul);
  ASSERT_EQ(error.value().line_offset, 1ul);
  ASSERT_EQ(error.value().line, "ghi");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
