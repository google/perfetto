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

#include "src/trace_processor/core/exec/hash_aggregate.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ReadColumn;
using test::ReadNullableColumn;
using testing::ElementsAre;

using F = AggregateCall::Function;

using Columns = std::vector<std::vector<std::optional<int64_t>>>;

// Runs `spec` over `batches`, ending a pipeline in it as a plan does. Returns
// each column of the one batch it outputs, none if it failed, with how it
// ended in `status`.
Columns Aggregate(core::Arena& arena,
                  HashAggregateSpec spec,
                  std::vector<const RowBatch*> batches,
                  base::Status* status = nullptr) {
  test::BatchesSource source(std::move(batches));
  test::AfterBreaker run(arena, source, {},
                         arena.New<HashAggregate>(arena, spec));
  Columns out;
  base::Status ended = run.Drain([&](const RowBatch& batch) {
    EXPECT_TRUE(out.empty());
    for (uint32_t c = 0; c < batch.column_count(); ++c) {
      out.push_back(ReadNullableColumn<int64_t>(batch, c));
    }
  });
  if (status) {
    *status = ended;
  } else {
    EXPECT_TRUE(ended.ok()) << ended.message();
  }
  return out;
}

TEST(HashAggregateTest, FoldsGroupsInTheOrderFirstSeen) {
  core::Arena arena;
  // Two batches of (key, value). In the second, both columns are nullable.
  std::vector<int64_t> keys = {3, 1, 3, 2, 0, 1, 0};
  std::vector<int64_t> values = {10, 20, 30, 40, 50, 60, 70};
  BitVector key_valid = BitVector::CreateWithSize(7, true);
  key_valid.clear(4);
  key_valid.clear(6);
  BitVector value_valid = BitVector::CreateWithSize(7, true);
  value_valid.clear(3);
  value_valid.clear(5);
  RowBatch in[2];
  for (uint32_t i = 0; i < 2; ++i) {
    uint32_t at = i == 0 ? 0 : 4;
    in[i].AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}},
                                                  keys.data(), &key_valid, at));
    in[i].AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, values.data(), &value_valid, at));
    in[i].SetRowCount(i == 0 ? 4 : 3);
  }
  Columns out = Aggregate(
      arena,
      HashAggregateSpec{test::InArena<uint32_t>(arena, {0}),
                        test::InArena<AggregateCall>(
                            arena, {{F::kCountStar, 0}, {F::kSum, 1}})},
      {&in[0], &in[1]});
  // Groups 3, 1, 2 and null. A sum of only nulls is null.
  EXPECT_THAT(out[0], ElementsAre(3, 1, 2, std::nullopt));
  EXPECT_THAT(out[1], ElementsAre(2, 2, 1, 2));
  EXPECT_THAT(out[2], ElementsAre(40, 20, std::nullopt, 120));
}

TEST(HashAggregateTest, NoKeysIsOneGroupEvenWithoutRows) {
  core::Arena arena;
  HashAggregateSpec spec{
      {},
      test::InArena<AggregateCall>(arena, {{F::kCountStar, 0}, {F::kSum, 0}})};
  // With no rows, COUNT is 0 and SUM is null.
  Columns out = Aggregate(arena, spec, {});
  EXPECT_THAT(out[0], ElementsAre(0));
  EXPECT_THAT(out[1], ElementsAre(std::nullopt));

  std::vector<int64_t> values = {4, 5, 6};
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetRowCount(3);
  out = Aggregate(arena, spec, {&in});
  EXPECT_THAT(out[0], ElementsAre(3));
  EXPECT_THAT(out[1], ElementsAre(15));
}

TEST(HashAggregateTest, NoKeysFoldsEachBatchAtOnce) {
  core::Arena arena;
  // Two batches of one nullable column, with rows 1 and 5 null.
  constexpr int64_t kHalf = std::numeric_limits<int64_t>::max() / 2;
  std::vector<int64_t> values = {-7, 5, kHalf, 0, -kHalf, 9, 3};
  BitVector valid = BitVector::CreateWithSize(7, true);
  valid.clear(1);
  valid.clear(5);
  RowBatch in[2];
  for (uint32_t i = 0; i < 2; ++i) {
    uint32_t at = i == 0 ? 0 : 4;
    in[i].AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, values.data(), &valid, at));
    in[i].SetRowCount(i == 0 ? 4 : 3);
  }
  Columns out = Aggregate(
      arena,
      HashAggregateSpec{{},
                        test::InArena<AggregateCall>(arena, {{F::kCountStar, 0},
                                                             {F::kCount, 0},
                                                             {F::kSum, 0},
                                                             {F::kMin, 0},
                                                             {F::kMax, 0}})},
      {&in[0], &in[1]});
  EXPECT_THAT(out[0], ElementsAre(7));
  EXPECT_THAT(out[1], ElementsAre(5));
  EXPECT_THAT(out[2], ElementsAre(-4));
  EXPECT_THAT(out[3], ElementsAre(-kHalf));
  EXPECT_THAT(out[4], ElementsAre(kHalf));
}

TEST(HashAggregateTest, NoKeysSumFailsPastInt64) {
  core::Arena arena;
  // The least Int64 fits; one less doesn't.
  std::vector<int64_t> least = {std::numeric_limits<int64_t>::min(), 0};
  std::vector<int64_t> one = {-1};
  RowBatch in[2];
  in[0].AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, least.data()));
  in[0].SetRowCount(2);
  in[1].AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, one.data()));
  in[1].SetRowCount(1);
  HashAggregateSpec spec{{},
                         test::InArena<AggregateCall>(arena, {{F::kSum, 0}})};
  base::Status status;
  Aggregate(arena, spec, {&in[0]}, &status);
  EXPECT_TRUE(status.ok());
  Aggregate(arena, spec, {&in[0], &in[1]}, &status);
  EXPECT_FALSE(status.ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
