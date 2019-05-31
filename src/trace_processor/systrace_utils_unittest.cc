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

#include "src/trace_processor/systrace_utils.h"

#include <gtest/gtest.h>

namespace perfetto {
namespace trace_processor {
namespace systrace_utils {

TEST(SystraceParserTest, SystraceEvent) {
  SystraceTracePoint result{};

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView(""), &result),
            SystraceParseResult::kFailure);

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("B|1|foo"), &result),
            SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (SystraceTracePoint('B', 1, base::StringView("foo"), 0)));

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("B|42|Bar"), &result),
            SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (SystraceTracePoint('B', 42, base::StringView("Bar"), 0)));

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("C|543|foo|"), &result),
            SystraceParseResult::kFailure);
  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("C|543|foo|8"), &result),
            SystraceParseResult::kSuccess);
  EXPECT_EQ(result, (SystraceTracePoint('C', 543, base::StringView("foo"), 8)));

  ASSERT_EQ(ParseSystraceTracePoint(base::StringView("S|"), &result),
            SystraceParseResult::kUnsupported);
}
}  // namespace systrace_utils
}  // namespace trace_processor
}  // namespace perfetto
