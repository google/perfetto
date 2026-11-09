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

#include "src/trace_processor/core/exec/sort.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

std::vector<uint32_t> Ids(const Source& source, base::Status* status) {
  std::unique_ptr<OperatorState> state = source.MakeState(test::TestContext());
  RowBatch batch;
  std::vector<uint32_t> ids;
  while (source.GetData(batch, *state)) {
    for (uint32_t id : test::ReadColumn<uint32_t>(batch, 0)) {
      ids.push_back(id);
    }
  }
  *status = source.status(*state);
  return ids;
}

// Every row a cursor reads: its id, and its value or nullopt for a null.
std::vector<std::pair<uint32_t, std::optional<int64_t>>> CursorRows(
    const Source& source) {
  RowCursor cursor(source);
  std::vector<std::pair<uint32_t, std::optional<int64_t>>> rows;
  for (bool row = cursor.Open(); row; row = cursor.Next()) {
    std::optional<int64_t> value;
    if (cursor.batch().column(1).IsValid(cursor.batch_row())) {
      value = cursor.Value<int64_t>(1);
    }
    rows.emplace_back(cursor.Value<uint32_t>(0), value);
  }
  EXPECT_TRUE(cursor.status().ok());
  return rows;
}

std::vector<Pipeline::Step> SortInPlace(std::vector<SortSpec::Key> keys,
                                        std::vector<bool> unread) {
  auto sort = std::make_unique<Sort>(SortSpec{std::move(keys)});
  sort->SetUnreadColumns(std::move(unread));
  sort->ServeInPlace();
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::move(sort));
  return ops;
}

std::vector<Pipeline::Step> SortBy(std::vector<SortSpec::Key> keys) {
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<Sort>(SortSpec{std::move(keys)}));
  return ops;
}

TEST(SortTest, RewindAfterInvalidKeysAndCompletedSort) {
  Sort sort(SortSpec{{{1, false}, {2, true}}});
  auto state = sort.MakeState(test::TestContext());
  std::vector<int64_t> first = {2, 1, 2, 1};
  std::vector<int32_t> narrower = {2, 1, 2, 1};
  std::vector<double> second = {0.5, -1.5, -0.5, 2.5};
  RowBatch in;
  RowBatch out;
  in.AddBorrowedColumn(ColumnView::Sequence(nullptr, 0));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, first.data()));
  // The first key is valid, but the second cannot be used in a row layout.
  in.AddBorrowedColumn(ColumnView::Variants(nullptr));
  in.SetRowCount(4);
  ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(sort.status(*state).message(), testing::HasSubstr("key 2"));

  for (bool change_type : {false, true}) {
    // Recover from the rejected batch, then reuse a completed sort with a
    // different key width. Both runs must establish their own row layout.
    state->EndRun();
    ASSERT_TRUE(sort.status(*state).ok());
    if (change_type) {
      in.SetColumn(1,
                   ColumnView::Reference(StorageType{Int32{}}, narrower.data()),
                   ColumnBuffer());
    }
    in.SetColumn(2, ColumnView::Reference(StorageType{Double{}}, second.data()),
                 ColumnBuffer());
    ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kNeedMoreInput);
    ASSERT_EQ(sort.Finish(out, *state), OpResult::kHaveMoreOutput);
    // Equal first keys are ordered by the descending second key.
    EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(3, 1, 0, 2));
    EXPECT_EQ(sort.Finish(out, *state), OpResult::kNeedMoreInput);
    EXPECT_TRUE(sort.status(*state).ok());
  }
}

// Strings sort by their contents, not by the order the pool interned them,
// with nulls first ascending and last descending.
// Rows sorted within one batch held are served where they are held, not
// copied.
TEST(SortTest, ServesRowsWhereTheyAreHeld) {
  std::vector<int64_t> values = {3, 1, 2};
  Sort sort(SortSpec{{{0, false}}});
  auto state = sort.MakeState(test::TestContext());
  RowBatch in;
  RowBatch out;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetRowCount(3);
  ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kNeedMoreInput);
  ASSERT_EQ(sort.Finish(out, *state), OpResult::kHaveMoreOutput);
  EXPECT_EQ(out.column(0).data(), values.data());
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 2, 3));
}

TEST(SortTest, StringsSortByTheirContents) {
  StringPool pool;
  std::vector<StringPool::Id> strings = {
      pool.InternString("b"), pool.InternString("c"), pool.InternString("a"),
      StringPool::Id::Null(), pool.InternString("b")};
  auto valid = BitVector::CreateWithSize(5, true);
  valid.clear(3);
  RowBatch in;
  in.AddBorrowedColumn(ColumnView::Sequence(nullptr, 0));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{String{}}, strings.data(), &valid));
  in.SetRowCount(5);
  for (bool descending : {false, true}) {
    Sort sort(SortSpec{{{1, descending}}, &pool});
    auto state = sort.MakeState(test::TestContext());
    RowBatch out;
    ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kNeedMoreInput);
    ASSERT_EQ(sort.Finish(out, *state), OpResult::kHaveMoreOutput);
    std::vector<uint32_t> expected = {3, 2, 0, 4, 1};
    if (descending) {
      expected = {1, 0, 4, 2, 3};
    }
    EXPECT_EQ(test::ReadColumn<uint32_t>(out, 0), expected);
  }
}

TEST(SortTest, IdsSortAsTheirValues) {
  test::ArraySource source({5, 4, 3});
  Pipeline sorted(source, SortBy({{0, true}}));
  base::Status status;
  EXPECT_THAT(Ids(sorted, &status), ElementsAre(2, 1, 0));
}

// Rows over several batches, served in place, read as they would be copied,
// on every run.
TEST(SortTest, ServesRowsInPlaceToACursor) {
  constexpr uint32_t kRows = 3 * kMaxBatchRows + 5;
  std::vector<int64_t> values;
  for (uint32_t i = 0; i < kRows; ++i) {
    values.push_back((i * 7919) % 1000);
  }
  test::ArraySource source(values);
  Pipeline copied(source, SortBy({{1, true}}));
  Pipeline in_place(source, SortInPlace({{1, true}}, {}));
  auto expected = CursorRows(copied);
  ASSERT_EQ(expected.size(), kRows);
  EXPECT_EQ(CursorRows(in_place), expected);

  RowCursor cursor(in_place);
  for (int run = 0; run < 2; ++run) {
    uint32_t i = 0;
    for (bool row = cursor.Open(); row; row = cursor.Next(), ++i) {
      ASSERT_EQ(cursor.Value<uint32_t>(0), expected[i].first);
    }
    EXPECT_EQ(i, kRows);
  }
}

// A column no later step reads reads back as nulls, in place too.
TEST(SortTest, ServesUnreadColumnsInPlaceAsNulls) {
  test::ArraySource source({3, 1, 2});
  Pipeline in_place(source, SortInPlace({{0, true}}, {false, true}));
  auto rows = CursorRows(in_place);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0], std::make_pair(2u, std::optional<int64_t>()));
  EXPECT_EQ(rows[1], std::make_pair(1u, std::optional<int64_t>()));
  EXPECT_EQ(rows[2], std::make_pair(0u, std::optional<int64_t>()));
}

// Lends two windows of a sparse column, as a dataframe scan does: an id
// column, then the sparse values, in which rows 1 and 4 are null.
class SparseSource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  SparseSource()
      : valid_(BitVector::CreateWithSize(6, true)),
        popcount_((valid_.clear(1),
                   valid_.clear(4),
                   valid_.PrefixPopcountFlexVector())) {}

  std::unique_ptr<OperatorState> MakeState(Context&) const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.emitted == 6) {
      return false;
    }
    ColumnView sparse = ColumnView::Sparse(StorageType{Int64{}}, values_.data(),
                                           &valid_, popcount_.data());
    sparse.set_start(s.emitted);
    out.Reset();
    out.AddBorrowedColumn(ColumnView::Sequence(nullptr, s.emitted));
    out.AddBorrowedColumn(sparse);
    out.SetRowCount(3);
    s.emitted += 3;
    return true;
  }

  // The value of a row read in place, or nullopt for a null.
  static std::optional<int64_t> Read(const RowCursor& cursor) {
    const ColumnView& view = cursor.batch().column(1);
    uint32_t slot = filter::IndexToStorageIndex<SparseNull>(
        view.Index(cursor.batch_row()), view.validity(), view.popcount());
    if (slot == std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<const int64_t*>(view.sparse_data())[slot];
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  std::vector<int64_t> values_ = {30, 10, 20, 5};
  BitVector valid_;
  FlexVector<uint32_t> popcount_;
};

// A sparse key sorts as its values, nulls first, and a sort serving in place
// holds the column sparse, so it is read where the scan lent it.
TEST(SortTest, ServesSparseColumnsInPlace) {
  SparseSource source;
  Pipeline in_place(source, SortInPlace({{1, false}}, {}));
  RowCursor cursor(in_place);
  std::vector<uint32_t> ids;
  std::vector<std::optional<int64_t>> values;
  for (bool row = cursor.Open(); row; row = cursor.Next()) {
    ASSERT_EQ(cursor.batch().column(1).kind(), ColumnView::Kind::kSparse);
    ids.push_back(cursor.Value<uint32_t>(0));
    values.push_back(SparseSource::Read(cursor));
  }
  ASSERT_TRUE(cursor.status().ok());
  EXPECT_THAT(ids, ElementsAre(1, 4, 5, 2, 3, 0));
  EXPECT_THAT(values, ElementsAre(std::nullopt, std::nullopt, 5, 10, 20, 30));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
