/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/exec/limit.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

std::vector<Pipeline::Step> Limited(std::optional<uint32_t> limit,
                                    uint32_t offset) {
  std::vector<Pipeline::Step> steps;
  steps.push_back(std::make_unique<Limit>(limit, offset));
  return steps;
}

std::vector<int64_t> Read(RowCursor& cursor) {
  std::vector<int64_t> values;
  for (bool more = cursor.Open(); more; more = cursor.Next()) {
    values.push_back(cursor.Value<int64_t>(1));
  }
  return values;
}

// The rows kept span batches, and each run counts from the first row.
TEST(LimitTest, KeepsTheRowsAfterTheOffset) {
  test::ArraySource source(test::Sequence(5000));
  Pipeline pipeline(source, Limited(2000, 2047));
  RowCursor cursor(pipeline);
  for (uint32_t run = 0; run < 2; ++run) {
    std::vector<int64_t> values = Read(cursor);
    ASSERT_EQ(values.size(), 2000u);
    EXPECT_EQ(values.front(), 2047);
    EXPECT_EQ(values.back(), 4046);
    EXPECT_TRUE(cursor.status().ok());
  }
}

// Once it has its rows, nothing more is read: the source fails if asked for
// a second batch.
TEST(LimitTest, StopsReadingOnceItHasItsRows) {
  test::FailingSource source;
  Pipeline enough(source, Limited(2, 0));
  RowCursor first(enough);
  EXPECT_THAT(Read(first), ElementsAre(1, 2));
  EXPECT_TRUE(first.status().ok());

  Pipeline more(source, Limited(3, 0));
  RowCursor second(more);
  Read(second);
  EXPECT_FALSE(second.status().ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
