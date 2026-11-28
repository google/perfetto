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
#include "src/trace_processor/core/util/arena.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

std::vector<Pipeline::Step> Limited(core::Arena& arena,
                                    std::optional<uint32_t> limit,
                                    uint32_t offset) {
  std::vector<Pipeline::Step> steps;
  steps.push_back(arena.New<Limit>(limit, offset));
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
  core::Arena arena;
  test::ArraySource source(arena, test::Sequence(5000));
  std::vector<Pipeline::Step> steps = Limited(arena, 2000, 2047);
  const Pipeline& pipeline = *arena.New<Pipeline>(arena, source, steps);
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
  core::Arena arena;
  test::FailingSource source;
  std::vector<Pipeline::Step> two = Limited(arena, 2, 0);
  const Pipeline& enough = *arena.New<Pipeline>(arena, source, two);
  RowCursor first(enough);
  EXPECT_THAT(Read(first), ElementsAre(1, 2));
  EXPECT_TRUE(first.status().ok());

  std::vector<Pipeline::Step> three = Limited(arena, 3, 0);
  const Pipeline& more = *arena.New<Pipeline>(arena, source, three);
  RowCursor second(more);
  Read(second);
  EXPECT_FALSE(second.status().ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
