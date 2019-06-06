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

#include "src/trace_processor/systrace_parser.h"

#include <gtest/gtest.h>

namespace perfetto {
namespace trace_processor {

TEST(SystraceParserTest, SystraceEvent) {
  systrace_utils::SystraceTracePoint result{};

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView(""), &result),
            systrace_utils::SystraceParseResult::kFailure);

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("B|1|foo"), &result),
            systrace_utils::SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (systrace_utils::SystraceTracePoint(
                        'B', 1, base::StringView("foo"), 0)));

  ASSERT_EQ(systrace_utils::ParseSystraceTracePoint(
                base::StringView("B|42|Bar"), &result),
            systrace_utils::SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (systrace_utils::SystraceTracePoint(
                        'B', 42, base::StringView("Bar"), 0)));

  ASSERT_EQ(systrace_utils::ParseSystraceTracePoint(
                base::StringView("C|543|foo|"), &result),
            systrace_utils::SystraceParseResult::kFailure);
  ASSERT_EQ(systrace_utils::ParseSystraceTracePoint(
                base::StringView("C|543|foo|8"), &result),
            systrace_utils::SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (systrace_utils::SystraceTracePoint(
                        'C', 543, base::StringView("foo"), 8)));

  ASSERT_EQ(
      systrace_utils::ParseSystraceTracePoint(base::StringView("S|"), &result),
      systrace_utils::SystraceParseResult::kUnsupported);
}

}  // namespace trace_processor
}  // namespace perfetto
