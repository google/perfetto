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

}  // namespace
}  // namespace protozero
