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

#include "src/trace_processor/core/util/ops.h"

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::ops {
namespace {

TEST(OpsTest, EstimateDistinctCount) {
  base::FlatHashMap<int64_t, uint32_t> counts;
  const int32_t values[] = {1, 2, 1, 3};
  EXPECT_EQ(EstimateDistinctCount(&counts, MakeSpan(values)), 3u);

  const StringPool::Id strings[] = {
      StringPool::Id::Raw(1), StringPool::Id::Raw(2), StringPool::Id::Raw(1),
      StringPool::Id::Raw(2)};
  EXPECT_EQ(EstimateDistinctCount(&counts, MakeSpan(strings)), 2u);
}

TEST(OpsTest, DistinctRows) {
  const uint32_t rows[] = {1, 2, 1, 3};
  uint32_t indices[] = {10, 20, 30, 40};
  Span<uint32_t> index_span = MakeMutableSpan(indices);
  DistinctRows(AsBytes(MakeSpan(rows)), sizeof(uint32_t), &index_span);
  EXPECT_THAT(index_span, testing::ElementsAre(10u, 20u, 40u));
}

TEST(OpsTest, SortRowLayoutStably) {
  const uint32_t rows[] = {2, 1, 2};
  uint32_t indices[] = {10, 20, 30};
  Span<uint32_t> index_span = MakeMutableSpan(indices);
  SortRowLayout(AsBytes(MakeSpan(rows)), sizeof(uint32_t), &index_span);
  EXPECT_THAT(index_span, testing::ElementsAre(20u, 10u, 30u));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::ops
