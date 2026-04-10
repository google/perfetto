/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/protozero/filtering/string_filter.h"

#include "protos/perfetto/trace/trace_packet.pb.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

TEST(StringFilterTest, RegexRedaction) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string res = "B|1234|foo 1234 bar baz";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo P60REDACTED-");
}

TEST(StringFilterTest, RegexRedactionShort) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string res = "B|1234|foo 1234";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo P60R");
}

TEST(StringFilterTest, RegexRedactionMismatch) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string res = "B|1234|fooo";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|fooo");
}

TEST(StringFilterTest, AtraceRegexRedaction) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo");

  std::string res = "B|1234|foo 1234 bar baz";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo P60REDACTED-");
}

TEST(StringFilterTest, AtraceRegexRedactionZero) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|(.*))", "");

  std::string res = "B|1234|";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|");
}

TEST(StringFilterTest, AtraceRegexRedactionExact) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo 1234 bar baz");

  std::string res = "B|1234|foo 1234 bar baz";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo P60REDACTED-");
}

TEST(StringFilterTest, AtraceRegexRedactionEmpty) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string res = "B|1234|foo 1234";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo P60R");
}

TEST(StringFilterTest, AtraceRegexRedactionTooLong) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo 1234 bar baz ");

  std::string res = "B|1234|foo 1234 bar baz";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo 1234 bar baz");
}

TEST(StringFilterTest, AtraceRegexRedactionMismatch) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo 2");

  std::string res = "B|1234|foo 1234 bar baz";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo 1234 bar baz");
}

TEST(StringFilterTest, AtraceRegexRedactionEnd) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups, R"(E\|\d+)",
                 "");

  std::string res = "E|1234";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "E|1234");
}

TEST(StringFilterTest, AtraceRegexRedactionNotAtrace) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups, R"(B\|\d+)",
                 "");

  std::string res = "B|1";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1");
}

TEST(StringFilterTest, AtraceRegexRedactionMultiple) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo");
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|(.*))", "bar");
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|R(.*))", "R");

  std::string res = "B|1|bar 1234567";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1|P60REDACTED");
}

TEST(StringFilterTest, Mixed) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "foo");
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(B\|\d+\|(.*))",
                 "");

  std::string res = "B|1234|foo";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|P60");
}

TEST(StringFilterTest, Break) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kMatchBreak, R"(B\|\d+)", "");
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(B\|(\d+))", "");

  std::string res = "B|1234";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234");
}

TEST(StringFilterTest, AtraceBreak) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchBreak, R"(B\|\d+|foo .*)",
                 "foo");
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|(\d+)|foo (.*))", "foo");

  std::string res = "B|1234|foo 1234";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo 1234");
}

TEST(StringFilterTest, AtraceSearch) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(x:(\d+))", "foo");

  std::string res = "B|1234|foo x:1234 x:494 y:4904 x:dfja x:239039";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo x:P60R x:P60 y:4904 x:dfja x:P60RED");
}

TEST(StringFilterTest, AtraceSearchBreaks) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(x:(\d+))", "foo");
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(y:(\d+))", "foo");

  std::string res = "B|1234|foo x:1234 x:494 y:4904 x:dfja x:239039";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo x:P60R x:P60 y:4904 x:dfja x:P60RED");
}

TEST(StringFilterTest, AtraceSearchReturnsFalseOnNoMatch) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(x:(\d+))", "foo");

  std::string res = "B|1234|foo x:dfja";
  ASSERT_FALSE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo x:dfja");
}

TEST(StringFilterTest, AtraceSearchMultipleGroups) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(x:(\d+)|y:(\d+))", "foo");

  std::string res = "B|1234|foo x:1234 x:494 y:4904 x:dfja x:239039";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo x:P60R x:P60 y:P60R x:dfja x:P60RED");
}

TEST(StringFilterTest, AtraceSearchRecursive) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                 R"(x:([^\s-]*))", "foo");

  std::string res = "B|1234|foo x:1234 x:494 y:4904 x:dfja x:239039";
  ASSERT_TRUE(filter.MaybeFilter(res.data(), res.size()));
  ASSERT_EQ(res, "B|1234|foo x:P60R x:P60 y:4904 x:P60R x:P60RED");
}

TEST(StringFilterTest, RegexRedactionNonUtf) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string bad = std::string(1, char(0xff));
  std::string bad_copy = bad;

  ASSERT_FALSE(filter.MaybeFilter(bad.data(), bad.size()));
  ASSERT_EQ(bad, bad_copy);

  perfetto::protos::TracePacket packet;
  packet.mutable_perfetto_metatrace()->set_counter_id(0);
  *packet.mutable_perfetto_metatrace()->mutable_counter_name() = "foo";
  packet.mutable_perfetto_metatrace()->set_counter_value(100);

  std::string metatrace = packet.SerializeAsString();
  std::string metatrace_copy = metatrace;

  ASSERT_FALSE(filter.MaybeFilter(metatrace.data(), metatrace.size()));
  ASSERT_EQ(metatrace, metatrace_copy);
}

TEST(StringFilterTest, AtraceRedactionNonUtf) {
  StringFilter filter;
  filter.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                 R"(B\|\d+\|foo (.*))", "");

  std::string bad = std::string(1, char(0xff));
  std::string bad_copy = bad;

  ASSERT_FALSE(filter.MaybeFilter(bad.data(), bad.size()));
  ASSERT_EQ(bad, bad_copy);

  perfetto::protos::TracePacket packet;
  packet.mutable_perfetto_metatrace()->set_counter_id(0);
  *packet.mutable_perfetto_metatrace()->mutable_counter_name() = "foo";
  packet.mutable_perfetto_metatrace()->set_counter_value(100);

  std::string metatrace = packet.SerializeAsString();
  std::string metatrace_copy = metatrace;

  ASSERT_FALSE(filter.MaybeFilter(metatrace.data(), metatrace.size()));
  ASSERT_EQ(metatrace, metatrace_copy);
}

TEST(StringFilterTest, SemanticTypeBasicMatching) {
  StringFilter filter;

  // Add rule for semantic type 1 (ATRACE)
  auto mask_type1 = StringFilter::SemanticTypeMask::FromWords(1ULL << 1, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(secret:(.*))", "",
                 "", mask_type1);

  // Add rule for semantic type 2 (JOB)
  auto mask_type2 = StringFilter::SemanticTypeMask::FromWords(1ULL << 2, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(password:(.*))",
                 "", "", mask_type2);

  // Test that rule 1 applies to type 1
  std::string str1 = "secret:value123";
  ASSERT_TRUE(filter.MaybeFilter(str1.data(), str1.size(), 1));
  ASSERT_EQ(str1, "secret:P60REDAC");

  // Test that rule 1 does not apply to type 2
  std::string str2 = "secret:value123";
  ASSERT_FALSE(filter.MaybeFilter(str2.data(), str2.size(), 2));
  ASSERT_EQ(str2, "secret:value123");

  // Test that rule 2 applies to type 2
  std::string str3 = "password:secret123";
  ASSERT_TRUE(filter.MaybeFilter(str3.data(), str3.size(), 2));
  ASSERT_EQ(str3, "password:P60REDACT");

  // Test that rule 2 does not apply to type 1
  std::string str4 = "password:secret123";
  ASSERT_FALSE(filter.MaybeFilter(str4.data(), str4.size(), 1));
  ASSERT_EQ(str4, "password:secret123");

  // Test that neither applies to type 3
  std::string str5 = "secret:value123";
  ASSERT_FALSE(filter.MaybeFilter(str5.data(), str5.size(), 3));
  ASSERT_EQ(str5, "secret:value123");

  std::string str6 = "password:secret123";
  ASSERT_FALSE(filter.MaybeFilter(str6.data(), str6.size(), 3));
  ASSERT_EQ(str6, "password:secret123");
}

TEST(StringFilterTest, SemanticTypeDefaultMask) {
  StringFilter filter;

  // Add rule without explicit semantic type mask (defaults to UNSPECIFIED only)
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(data:(.*))", "");

  // Verify rule applies to semantic type 0 (UNSPECIFIED)
  std::string str0 = "data:value0";
  ASSERT_TRUE(filter.MaybeFilter(str0.data(), str0.size(), 0));
  ASSERT_EQ(str0, "data:P60RED");

  // Verify rule does NOT apply to semantic type 1 (default is UNSPECIFIED only)
  std::string str1 = "data:value1";
  ASSERT_FALSE(filter.MaybeFilter(str1.data(), str1.size(), 1));
  ASSERT_EQ(str1, "data:value1");

  // Verify rule does NOT apply to semantic type 2
  std::string str2 = "data:value2";
  ASSERT_FALSE(filter.MaybeFilter(str2.data(), str2.size(), 2));
  ASSERT_EQ(str2, "data:value2");
}

TEST(StringFilterTest, SemanticTypeExplicitMultipleTypes) {
  StringFilter filter;

  // Add rule with explicit mask for types 0, 1, and 2
  auto mask = StringFilter::SemanticTypeMask::FromWords(0x7, 0);  // bits 0,1,2
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(data:(.*))", "",
                 "", mask);

  // Verify rule applies to semantic type 0
  std::string str0 = "data:value0";
  ASSERT_TRUE(filter.MaybeFilter(str0.data(), str0.size(), 0));
  ASSERT_EQ(str0, "data:P60RED");

  // Verify rule applies to semantic type 1
  std::string str1 = "data:value1";
  ASSERT_TRUE(filter.MaybeFilter(str1.data(), str1.size(), 1));
  ASSERT_EQ(str1, "data:P60RED");

  // Verify rule applies to semantic type 2
  std::string str2 = "data:value2";
  ASSERT_TRUE(filter.MaybeFilter(str2.data(), str2.size(), 2));
  ASSERT_EQ(str2, "data:P60RED");

  // Verify rule does NOT apply to semantic type 3 (not in mask)
  std::string str3 = "data:value3";
  ASSERT_FALSE(filter.MaybeFilter(str3.data(), str3.size(), 3));
  ASSERT_EQ(str3, "data:value3");
}

TEST(StringFilterTest, SemanticTypeMultipleRules) {
  StringFilter filter;

  // Add rule for type 1 only
  auto mask_type1 = StringFilter::SemanticTypeMask::FromWords(1ULL << 1, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(foo:(.*))", "",
                 "", mask_type1);

  // Add rule for type 2 only
  auto mask_type2 = StringFilter::SemanticTypeMask::FromWords(1ULL << 2, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(bar:(.*))", "",
                 "", mask_type2);

  // Add rule for types 1 and 2
  auto mask_type1_and_2 =
      StringFilter::SemanticTypeMask::FromWords((1ULL << 1) | (1ULL << 2), 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(baz:(.*))", "",
                 "", mask_type1_and_2);

  // Test string with type 1: only foo and baz rules should apply
  std::string str1a = "foo:secret";
  ASSERT_TRUE(filter.MaybeFilter(str1a.data(), str1a.size(), 1));
  ASSERT_EQ(str1a, "foo:P60RED");

  std::string str1b = "bar:secret";
  ASSERT_FALSE(filter.MaybeFilter(str1b.data(), str1b.size(), 1));
  ASSERT_EQ(str1b, "bar:secret");

  std::string str1c = "baz:secret";
  ASSERT_TRUE(filter.MaybeFilter(str1c.data(), str1c.size(), 1));
  ASSERT_EQ(str1c, "baz:P60RED");

  // Test string with type 2: only bar and baz rules should apply
  std::string str2a = "foo:secret";
  ASSERT_FALSE(filter.MaybeFilter(str2a.data(), str2a.size(), 2));
  ASSERT_EQ(str2a, "foo:secret");

  std::string str2b = "bar:secret";
  ASSERT_TRUE(filter.MaybeFilter(str2b.data(), str2b.size(), 2));
  ASSERT_EQ(str2b, "bar:P60RED");

  std::string str2c = "baz:secret";
  ASSERT_TRUE(filter.MaybeFilter(str2c.data(), str2c.size(), 2));
  ASSERT_EQ(str2c, "baz:P60RED");
}

TEST(StringFilterTest, SemanticTypeZero) {
  StringFilter filter;

  // Add rule for type 1 only
  auto mask_type1 = StringFilter::SemanticTypeMask::FromWords(1ULL << 1, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(type1:(.*))", "",
                 "", mask_type1);

  // Add rule with default mask (applies to all types including 0)
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(all:(.*))", "");

  // Test with semantic type 0 (unspecified) - UNSPECIFIED is its own category.
  // Type-specific rules do NOT apply to UNSPECIFIED.
  std::string str1 = "type1:value";
  ASSERT_FALSE(filter.MaybeFilter(str1.data(), str1.size(), 0));
  ASSERT_EQ(str1, "type1:value");

  // But rules with default mask (all bits set) still apply to type 0
  std::string str2 = "all:value";
  ASSERT_TRUE(filter.MaybeFilter(str2.data(), str2.size(), 0));
  ASSERT_EQ(str2, "all:P60RE");
}

TEST(StringFilterTest, SemanticTypeEdgeCases) {
  StringFilter filter;

  // Test semantic type 63 (boundary of first word)
  auto mask_63 = StringFilter::SemanticTypeMask::FromWords(1ULL << 63, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t63:(.*))", "",
                 "", mask_63);

  // Test semantic type 64 (boundary of second word)
  auto mask_64 = StringFilter::SemanticTypeMask::FromWords(0, 1ULL);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t64:(.*))", "",
                 "", mask_64);

  // Test semantic type 127 (maximum supported)
  auto mask_127 = StringFilter::SemanticTypeMask::FromWords(0, 1ULL << 63);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t127:(.*))", "",
                 "", mask_127);

  // Verify type 63
  std::string str63 = "t63:value";
  ASSERT_TRUE(filter.MaybeFilter(str63.data(), str63.size(), 63));
  ASSERT_EQ(str63, "t63:P60RE");

  std::string str63_wrong = "t63:value";
  ASSERT_FALSE(filter.MaybeFilter(str63_wrong.data(), str63_wrong.size(), 62));
  ASSERT_EQ(str63_wrong, "t63:value");

  // Verify type 64
  std::string str64 = "t64:value";
  ASSERT_TRUE(filter.MaybeFilter(str64.data(), str64.size(), 64));
  ASSERT_EQ(str64, "t64:P60RE");

  std::string str64_wrong = "t64:value";
  ASSERT_FALSE(filter.MaybeFilter(str64_wrong.data(), str64_wrong.size(), 63));
  ASSERT_EQ(str64_wrong, "t64:value");

  // Verify type 127
  std::string str127 = "t127:value";
  ASSERT_TRUE(filter.MaybeFilter(str127.data(), str127.size(), 127));
  ASSERT_EQ(str127, "t127:P60RE");

  std::string str127_wrong = "t127:value";
  ASSERT_FALSE(
      filter.MaybeFilter(str127_wrong.data(), str127_wrong.size(), 126));
  ASSERT_EQ(str127_wrong, "t127:value");

  // Test semantic type >= 128 (beyond supported range)
  // According to implementation, these should apply rules as safe default
  std::string str128 = "t127:value";
  ASSERT_TRUE(filter.MaybeFilter(str128.data(), str128.size(), 128));
  ASSERT_EQ(str128, "t127:P60RE");
}

TEST(StringFilterTest, SemanticTypeWithPolicies) {
  StringFilter filter;
  auto mask_type1 = StringFilter::SemanticTypeMask::FromWords(1ULL << 1, 0);

  // Test kMatchRedactGroups
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(match:(.*))", "",
                 "", mask_type1);
  std::string str1 = "match:secret";
  ASSERT_TRUE(filter.MaybeFilter(str1.data(), str1.size(), 1));
  ASSERT_EQ(str1, "match:P60RED");

  // Test kAtraceMatchRedactGroups
  StringFilter filter2;
  filter2.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                  R"(B\|\d+\|atrace (.*))", "atrace", "", mask_type1);
  std::string str2 = "B|1234|atrace secret";
  ASSERT_TRUE(filter2.MaybeFilter(str2.data(), str2.size(), 1));
  ASSERT_EQ(str2, "B|1234|atrace P60RED");

  std::string str2_wrong = "B|1234|atrace secret";
  ASSERT_FALSE(filter2.MaybeFilter(str2_wrong.data(), str2_wrong.size(), 2));
  ASSERT_EQ(str2_wrong, "B|1234|atrace secret");

  // Test kMatchBreak
  StringFilter filter3;
  auto mask_type2 = StringFilter::SemanticTypeMask::FromWords(1ULL << 2, 0);
  filter3.AddRule(StringFilter::Policy::kMatchBreak, R"(break:.*)", "", "",
                  mask_type1);
  filter3.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(break:(.*))", "",
                  "", mask_type2);
  std::string str3 = "break:value";
  ASSERT_FALSE(filter3.MaybeFilter(str3.data(), str3.size(), 1));
  ASSERT_EQ(str3, "break:value");

  std::string str3_other = "break:value";
  ASSERT_TRUE(filter3.MaybeFilter(str3_other.data(), str3_other.size(), 2));
  ASSERT_EQ(str3_other, "break:P60RE");

  // Test kAtraceMatchBreak
  StringFilter filter4;
  filter4.AddRule(StringFilter::Policy::kAtraceMatchBreak,
                  R"(B\|\d+\|abreak .*)", "abreak", "", mask_type1);
  filter4.AddRule(StringFilter::Policy::kAtraceMatchRedactGroups,
                  R"(B\|\d+\|abreak (.*))", "abreak", "", mask_type2);
  std::string str4 = "B|1234|abreak value";
  ASSERT_FALSE(filter4.MaybeFilter(str4.data(), str4.size(), 1));
  ASSERT_EQ(str4, "B|1234|abreak value");

  std::string str4_other = "B|1234|abreak value";
  ASSERT_TRUE(filter4.MaybeFilter(str4_other.data(), str4_other.size(), 2));
  ASSERT_EQ(str4_other, "B|1234|abreak P60RE");

  // Test kAtraceRepeatedSearchRedactGroups
  StringFilter filter5;
  filter5.AddRule(StringFilter::Policy::kAtraceRepeatedSearchRedactGroups,
                  R"(x:(\d+))", "search", "", mask_type1);
  std::string str5 = "B|1234|search x:123 x:456";
  ASSERT_TRUE(filter5.MaybeFilter(str5.data(), str5.size(), 1));
  ASSERT_EQ(str5, "B|1234|search x:P60 x:P60");

  std::string str5_wrong = "B|1234|search x:123 x:456";
  ASSERT_FALSE(filter5.MaybeFilter(str5_wrong.data(), str5_wrong.size(), 2));
  ASSERT_EQ(str5_wrong, "B|1234|search x:123 x:456");
}

TEST(StringFilterTest, RuleReplacementByName) {
  StringFilter filter;

  // Add rule with name "my_rule" that redacts "foo:(.*)"
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(foo:(.*))", "",
                 "my_rule");

  std::string str1 = "foo:secret";
  ASSERT_TRUE(filter.MaybeFilter(str1.data(), str1.size()));
  ASSERT_EQ(str1, "foo:P60RED");

  // bar pattern shouldn't match yet
  std::string str2 = "bar:secret";
  ASSERT_FALSE(filter.MaybeFilter(str2.data(), str2.size()));
  ASSERT_EQ(str2, "bar:secret");

  // Add another rule with name "my_rule" that redacts "bar:(.*)"
  // This should replace the first rule
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(bar:(.*))", "",
                 "my_rule");

  // Now foo shouldn't match (rule was replaced)
  std::string str3 = "foo:secret";
  ASSERT_FALSE(filter.MaybeFilter(str3.data(), str3.size()));
  ASSERT_EQ(str3, "foo:secret");

  // But bar should match
  std::string str4 = "bar:secret";
  ASSERT_TRUE(filter.MaybeFilter(str4.data(), str4.size()));
  ASSERT_EQ(str4, "bar:P60RED");

  // Test rules without names are not replaced
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(baz:(.*))", "");
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(qux:(.*))", "");

  // Both should match (no replacement occurred)
  std::string str5 = "baz:secret";
  ASSERT_TRUE(filter.MaybeFilter(str5.data(), str5.size()));
  ASSERT_EQ(str5, "baz:P60RED");

  std::string str6 = "qux:secret";
  ASSERT_TRUE(filter.MaybeFilter(str6.data(), str6.size()));
  ASSERT_EQ(str6, "qux:P60RED");
}

TEST(StringFilterTest, SemanticTypeMaskConstruction) {
  StringFilter filter;

  // Mask with bit 0 set
  auto mask_0 = StringFilter::SemanticTypeMask::FromWords(1, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t0:(.*))", "", "",
                 mask_0);

  // Mask with bit 63 set
  auto mask_63 = StringFilter::SemanticTypeMask::FromWords(1ULL << 63, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t63:(.*))", "",
                 "", mask_63);

  // Mask with bit 64 set
  auto mask_64 = StringFilter::SemanticTypeMask::FromWords(0, 1);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t64:(.*))", "",
                 "", mask_64);

  // Mask with bit 127 set
  auto mask_127 = StringFilter::SemanticTypeMask::FromWords(0, 1ULL << 63);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(t127:(.*))", "",
                 "", mask_127);

  // Mask with multiple bits: bits 0, 1, 64, 66
  auto mask_multi = StringFilter::SemanticTypeMask::FromWords(0x3, 0x5);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(multi:(.*))", "",
                 "", mask_multi);

  // Verify type 0
  std::string str0 = "t0:val";
  ASSERT_TRUE(filter.MaybeFilter(str0.data(), str0.size(), 0));
  ASSERT_EQ(str0, "t0:P60");

  // Verify type 63
  std::string str63 = "t63:val";
  ASSERT_TRUE(filter.MaybeFilter(str63.data(), str63.size(), 63));
  ASSERT_EQ(str63, "t63:P60");

  // Verify type 64
  std::string str64 = "t64:val";
  ASSERT_TRUE(filter.MaybeFilter(str64.data(), str64.size(), 64));
  ASSERT_EQ(str64, "t64:P60");

  // Verify type 127
  std::string str127 = "t127:val";
  ASSERT_TRUE(filter.MaybeFilter(str127.data(), str127.size(), 127));
  ASSERT_EQ(str127, "t127:P60");

  // Verify multi-bit mask applies to types 0, 1, 64, 66
  std::string multi0 = "multi:val";
  ASSERT_TRUE(filter.MaybeFilter(multi0.data(), multi0.size(), 0));
  ASSERT_EQ(multi0, "multi:P60");

  std::string multi1 = "multi:val";
  ASSERT_TRUE(filter.MaybeFilter(multi1.data(), multi1.size(), 1));
  ASSERT_EQ(multi1, "multi:P60");

  std::string multi2 = "multi:val";
  ASSERT_FALSE(filter.MaybeFilter(multi2.data(), multi2.size(), 2));
  ASSERT_EQ(multi2, "multi:val");

  std::string multi64 = "multi:val";
  ASSERT_TRUE(filter.MaybeFilter(multi64.data(), multi64.size(), 64));
  ASSERT_EQ(multi64, "multi:P60");

  std::string multi66 = "multi:val";
  ASSERT_TRUE(filter.MaybeFilter(multi66.data(), multi66.size(), 66));
  ASSERT_EQ(multi66, "multi:P60");

  std::string multi65 = "multi:val";
  ASSERT_FALSE(filter.MaybeFilter(multi65.data(), multi65.size(), 65));
  ASSERT_EQ(multi65, "multi:val");
}

// UNSPECIFIED (0) is treated as its own distinct category. A rule with
// a specific semantic type mask does NOT apply to UNSPECIFIED fields
// unless the mask explicitly includes bit 0.
TEST(StringFilterTest, UnspecifiedIsItsOwnCategory) {
  StringFilter filter;

  // Add rule that targets semantic type 1 (ATRACE) - does NOT include bit 0
  auto mask_atrace = StringFilter::SemanticTypeMask::FromWords(1ULL << 1, 0);
  filter.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(secret:(.*))", "",
                 "", mask_atrace);

  // Rule applies to semantic_type=1 (ATRACE)
  std::string str1 = "secret:value";
  ASSERT_TRUE(filter.MaybeFilter(str1.data(), str1.size(), 1));
  ASSERT_EQ(str1, "secret:P60RE");

  // Rule does NOT apply to semantic_type=0 (UNSPECIFIED) - it's its own
  // category
  std::string str2 = "secret:value";
  ASSERT_FALSE(filter.MaybeFilter(str2.data(), str2.size(), 0));
  ASSERT_EQ(str2, "secret:value");

  // Rule does NOT apply to semantic_type=2 (different type, not in mask)
  std::string str3 = "secret:value";
  ASSERT_FALSE(filter.MaybeFilter(str3.data(), str3.size(), 2));
  ASSERT_EQ(str3, "secret:value");

  // Add a second filter with rule that explicitly includes UNSPECIFIED (bit 0)
  StringFilter filter2;
  auto mask_with_unspecified =
      StringFilter::SemanticTypeMask::FromWords((1ULL << 0) | (1ULL << 1), 0);
  filter2.AddRule(StringFilter::Policy::kMatchRedactGroups, R"(secret:(.*))",
                  "", "", mask_with_unspecified);

  // This rule applies to both type 0 and type 1
  std::string str4 = "secret:value";
  ASSERT_TRUE(filter2.MaybeFilter(str4.data(), str4.size(), 0));
  ASSERT_EQ(str4, "secret:P60RE");

  std::string str5 = "secret:value";
  ASSERT_TRUE(filter2.MaybeFilter(str5.data(), str5.size(), 1));
  ASSERT_EQ(str5, "secret:P60RE");
}

}  // namespace
}  // namespace protozero
