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

#include "src/trace_processor/core/exec/top_k.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sort.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::vector<uint32_t> Ids(const Source& source) {
  std::unique_ptr<OperatorState> state = source.MakeState(test::TestContext());
  RowBatch batch;
  std::vector<uint32_t> ids;
  while (source.GetData(batch, *state)) {
    for (uint32_t id : test::ReadColumn<uint32_t>(batch, 0)) {
      ids.push_back(id);
    }
  }
  EXPECT_TRUE(source.status(*state).ok());
  return ids;
}

std::vector<uint32_t> RunOn(const Source& source,
                            std::unique_ptr<Operator> op) {
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::move(op));
  return Ids(Pipeline(source, std::move(ops)));
}

std::vector<uint32_t> FirstSorted(const Source& source,
                                  std::vector<SortKey> keys,
                                  uint32_t count) {
  std::vector<uint32_t> ids =
      RunOn(source, std::make_unique<Sort>(SortSpec{std::move(keys)}));
  ids.resize(std::min<size_t>(count, ids.size()));
  return ids;
}

TEST(TopKTest, OutputsTheFirstRowsAsSortOrdersThem) {
  constexpr uint32_t kRows = kMaxBatchRows * 5 + 7;
  std::vector<int64_t> values(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    // Many ties, so the rows kept must be the ones a stable sort puts first.
    values[i] = (i * 7919) % 101;
  }
  test::ArraySource source(values);
  for (uint32_t count : {0u, 1u, 3u, kMaxBatchRows + 5, kRows, kRows + 10}) {
    for (bool descending : {false, true}) {
      EXPECT_EQ(RunOn(source, std::make_unique<TopK>(
                                  TopKSpec{{{1, descending}}, nullptr, count})),
                FirstSorted(source, {{1, descending}}, count))
          << "count " << count << " descending " << descending;
    }
  }
}

// Every row of a descending input comes before the cutoff, so it prunes on
// every batch.
TEST(TopKTest, OutputsTheFirstRowsOfADescendingInput) {
  constexpr uint32_t kRows = kMaxBatchRows * 4;
  std::vector<int64_t> values(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    values[i] = kRows - i;
  }
  test::ArraySource source(values);
  EXPECT_EQ(RunOn(source,
                  std::make_unique<TopK>(TopKSpec{{{1, false}}, nullptr, 10})),
            FirstSorted(source, {{1, false}}, 10));
}

struct Column {
  StorageType type;
  const void* data;
  const BitVector* validity = nullptr;
};

// Adds to `ids` the ids `op` outputs from `rows` rows of an id column then
// `columns`. Returns whether it succeeded.
bool RunBatchesInto(Operator& op,
                    uint32_t rows,
                    const std::vector<Column>& columns,
                    std::vector<uint32_t>* ids) {
  auto state = op.MakeState(test::TestContext());
  RowBatch out;
  for (uint32_t at = 0; at < rows; at += kMaxBatchRows) {
    RowBatch in;
    in.AddBorrowedColumn(ColumnView::Sequence(nullptr, at));
    for (const Column& column : columns) {
      in.AddBorrowedColumn(
          ColumnView::Reference(column.type, column.data, column.validity, at));
    }
    in.SetRowCount(std::min(kMaxBatchRows, rows - at));
    if (op.Execute(in, out, *state) != OpResult::kNeedMoreInput) {
      break;
    }
  }
  while (op.Finish(out, *state) == OpResult::kHaveMoreOutput) {
    for (uint32_t id : test::ReadColumn<uint32_t>(out, 0)) {
      ids->push_back(id);
    }
  }
  return op.status(*state).ok();
}

std::vector<uint32_t> RunBatches(Operator& op,
                                 uint32_t rows,
                                 const std::vector<Column>& columns) {
  std::vector<uint32_t> ids;
  EXPECT_TRUE(RunBatchesInto(op, rows, columns, &ids));
  return ids;
}

// Strings, a few of them null, with many ties.
std::vector<StringPool::Id> Strings(StringPool& pool,
                                    uint32_t rows,
                                    BitVector* valid) {
  std::vector<StringPool::Id> strings(rows);
  *valid = BitVector::CreateWithSize(rows, true);
  for (uint32_t i = 0; i < rows; ++i) {
    if (i % 97 == 5) {
      strings[i] = StringPool::Id::Null();
      valid->clear(i);
      continue;
    }
    strings[i] = pool.InternString(
        base::StringView(std::to_string((rows - i) * 31 % 977)));
  }
  return strings;
}

// String keys are compared to the cutoff by their contents, nulls first
// ascending and last descending, as Sort orders them.
TEST(TopKTest, OutputsTheFirstStrings) {
  StringPool pool;
  constexpr uint32_t kRows = kMaxBatchRows * 3;
  BitVector valid;
  std::vector<StringPool::Id> strings = Strings(pool, kRows, &valid);
  std::vector<Column> columns = {
      {StorageType{String{}}, strings.data(), &valid}};
  for (uint32_t count : {1u, 10u, 40u}) {
    for (bool descending : {false, true}) {
      Sort sort(SortSpec{{{1, descending}}, &pool});
      std::vector<uint32_t> expected = RunBatches(sort, kRows, columns);
      expected.resize(count);
      TopK top(TopKSpec{{{1, descending}}, &pool, count});
      EXPECT_EQ(RunBatches(top, kRows, columns), expected)
          << "count " << count << " descending " << descending;
    }
  }
}

// With a string key and a number key, each is compared to the cutoff's in
// turn.
TEST(TopKTest, OutputsTheFirstRowsByStringAndNumberKeys) {
  StringPool pool;
  constexpr uint32_t kRows = kMaxBatchRows * 6;
  BitVector valid;
  std::vector<StringPool::Id> strings = Strings(pool, kRows, &valid);
  std::vector<int64_t> numbers(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    numbers[i] = (i * 7919) % 13;
  }
  std::vector<Column> columns = {
      {StorageType{String{}}, strings.data(), &valid},
      {StorageType{Int64{}}, numbers.data()}};
  std::vector<std::vector<SortKey>> orders = {{{1, false}, {2, true}},
                                              {{2, false}, {1, true}},
                                              {{1, true}, {2, false}}};
  for (const std::vector<SortKey>& keys : orders) {
    for (uint32_t count : {1u, 50u, 3000u}) {
      Sort sort(SortSpec{keys, &pool});
      std::vector<uint32_t> expected = RunBatches(sort, kRows, columns);
      expected.resize(count);
      TopK top(TopKSpec{keys, &pool, count});
      EXPECT_EQ(RunBatches(top, kRows, columns), expected)
          << "count " << count << " first key " << keys[0].column;
    }
  }
}

// Pruning appends the rows kept again, with a column no later step reads
// held as nulls.
TEST(TopKTest, PrunesRowsWithAColumnNotHeld) {
  constexpr uint32_t kRows = kMaxBatchRows * 4;
  std::vector<int64_t> keys(kRows);
  for (uint32_t i = 0; i < kRows; ++i) {
    keys[i] = (i * 7919) % 1009;
  }
  std::vector<Column> columns = {{StorageType{Int64{}}, keys.data()},
                                 {StorageType{Int64{}}, keys.data()}};
  Sort sort(SortSpec{{{1, false}}});
  std::vector<uint32_t> expected = RunBatches(sort, kRows, columns);
  expected.resize(5);
  TopK top(TopKSpec{{{1, false}}, nullptr, 5});
  top.SetUnreadColumns({false, false, true});
  EXPECT_EQ(RunBatches(top, kRows, columns), expected);
}

// No row is output, but a key which cannot be sorted by is still an error.
TEST(TopKTest, ChecksTheKeysWhenOutputtingNoRows) {
  StringPool pool;
  std::vector<StringPool::Id> strings(4, pool.InternString("a"));
  // Without a pool, a string key cannot be sorted by.
  TopK top(TopKSpec{{{1, false}}, nullptr, 0});
  std::vector<uint32_t> ids;
  EXPECT_FALSE(
      RunBatchesInto(top, 4, {{StorageType{String{}}, strings.data()}}, &ids));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
