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

#include "src/trace_processor/core/exec/aggregate.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/group_by.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

using Cell = std::optional<int64_t>;

// Rows of (key, value), either of which may be null, in batches of `chunk`.
// When `grouped`, a third column numbers each run of rows sharing a key, as
// GroupBy would for rows already next to each other.
class RowSource final : public Source {
 public:
  RowSource(std::vector<std::pair<Cell, Cell>> rows,
            uint32_t chunk,
            bool grouped = true)
      : chunk_(chunk),
        grouped_(grouped),
        key_valid_(
            BitVector::CreateWithSize(static_cast<uint32_t>(rows.size()))),
        value_valid_(
            BitVector::CreateWithSize(static_cast<uint32_t>(rows.size()))) {
    for (uint32_t i = 0; i < rows.size(); ++i) {
      keys_.push_back(rows[i].first.value_or(0));
      values_.push_back(rows[i].second.value_or(0));
      key_valid_.change(i, rows[i].first.has_value());
      value_valid_.change(i, rows[i].second.has_value());
      bool starts = i == 0 || rows[i].first != rows[i - 1].first;
      groups_.push_back(i == 0 ? 0 : groups_.back() + starts);
    }
  }

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(keys_.size());
    if (s.emitted == rows) {
      return false;
    }
    uint32_t count = std::min(chunk_, rows - s.emitted);
    out.Reset();
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, keys_.data(), &key_valid_));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values_.data(),
                                        &value_valid_));
    if (grouped_) {
      out.AddColumn(
          ColumnView::Reference(StorageType{Uint32{}}, groups_.data()));
    }
    out.Compose(RowSelection::Range(s.emitted), count);
    out.SetCardinality(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  uint32_t chunk_;
  bool grouped_;
  std::vector<int64_t> keys_;
  std::vector<int64_t> values_;
  std::vector<uint32_t> groups_;
  BitVector key_valid_;
  BitVector value_valid_;
};

struct Output {
  std::vector<Cell> keys;
  std::vector<int64_t> counts;
  std::vector<Cell> sums;
  base::Status status;
};

// Reads (key, COUNT(*), SUM(value)) rows, or without keys just the last two.
Output Drain(const Source& source, OperatorState& state, bool keyed = true) {
  RowBatch batch;
  Output out;
  uint32_t first = keyed ? 1 : 0;
  while (source.GetData(batch, state)) {
    EXPECT_LE(batch.size(), kMaxBatchRows);
    if (keyed) {
      for (Cell key : test::ReadNullableColumn<int64_t>(batch, 0)) {
        out.keys.push_back(key);
      }
    }
    for (int64_t count : test::ReadColumn<int64_t>(batch, first)) {
      out.counts.push_back(count);
    }
    for (Cell sum : test::ReadNullableColumn<int64_t>(batch, first + 1)) {
      out.sums.push_back(sum);
    }
  }
  out.status = source.status(state);
  return out;
}

Output Drain(const Source& source, bool keyed = true) {
  std::unique_ptr<OperatorState> state = source.MakeState();
  return Drain(source, *state, keyed);
}

// COUNT(*) and SUM(value) of rows grouped on their key, or of all rows. The
// group number is the column after (key, value).
std::vector<Pipeline::Step> CountAndSum(bool keyed = true) {
  AggregateSpec spec;
  if (keyed) {
    spec.key_columns = {0};
    spec.group_column = 2;
  }
  spec.aggregates = {{AggregateSpec::Function::kCount, 0},
                     {AggregateSpec::Function::kSum, 1}};
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<Aggregate>(std::move(spec)));
  return ops;
}

TEST(AggregateTest, FoldsEachRunOfAGroupAcrossBatches) {
  for (uint32_t chunk : {1u, 2u, 4u, kMaxBatchRows}) {
    SCOPED_TRACE(chunk);
    RowSource source(
        {{7, 1}, {7, 3}, {7, 6}, {3, 2}, {3, std::nullopt}, {5, 4}}, chunk);
    Pipeline aggregated(source, CountAndSum(), {});
    Output out = Drain(aggregated);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_THAT(out.keys, ElementsAre(7, 3, 5));
    EXPECT_THAT(out.counts, ElementsAre(3, 2, 1));
    EXPECT_THAT(out.sums, ElementsAre(10, 2, 4));
  }
}

TEST(AggregateTest, HoldsAGroupAcrossBatchesWhoseBuffersAreReused) {
  // A pipeline joins small batches before a fold, so drive it by hand: a
  // group can then span batches, end at the end of one, or fill one alone.
  const std::vector<std::pair<int64_t, int64_t>> rows = {
      {7, 1}, {7, 3}, {7, 6}, {7, 2}, {3, 2}, {3, 5}, {5, 4}};
  const std::vector<uint32_t> group_of = {0, 0, 0, 0, 1, 1, 2};
  AggregateSpec spec;
  spec.key_columns = {0};
  spec.group_column = 2;
  spec.aggregates = {{AggregateSpec::Function::kCount, 0},
                     {AggregateSpec::Function::kSum, 1}};
  Aggregate op(spec);
  auto state = op.MakeState();
  for (uint32_t chunk : {1u, 2u, 3u, 4u, 7u}) {
    SCOPED_TRACE(chunk);
    state->Reset();
    std::vector<int64_t> keys(chunk), values(chunk);
    std::vector<uint32_t> groups(chunk);
    RowBatch in, out;
    in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, keys.data()));
    in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
    in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, groups.data()));
    Output got;
    auto collect = [&] {
      if (!out.size()) {
        return;
      }
      for (int64_t key : test::ReadColumn<int64_t>(out, 0)) {
        got.keys.push_back(key);
      }
      for (int64_t count : test::ReadColumn<int64_t>(out, 1)) {
        got.counts.push_back(count);
      }
      for (Cell sum : test::ReadNullableColumn<int64_t>(out, 2)) {
        got.sums.push_back(sum);
      }
    };
    for (uint32_t at = 0; at < rows.size(); at += chunk) {
      auto count =
          static_cast<uint32_t>(std::min<size_t>(chunk, rows.size() - at));
      for (uint32_t row = 0; row < count; ++row) {
        keys[row] = rows[at + row].first;
        values[row] = rows[at + row].second;
        groups[row] = group_of[at + row];
      }
      in.SetCardinality(count);
      ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput)
          << op.status(*state).message();
      collect();
    }
    ASSERT_EQ(op.Finish(out, *state), OpResult::kNeedMoreInput);
    collect();
    EXPECT_THAT(got.keys, ElementsAre(7, 3, 5));
    EXPECT_THAT(got.counts, ElementsAre(4, 2, 1));
    EXPECT_THAT(got.sums, ElementsAre(12, 7, 4));
    EXPECT_EQ(op.Finish(out, *state), OpResult::kNeedMoreInput);
    EXPECT_EQ(out.size(), 0u);
  }
}

TEST(AggregateTest, NullKeysAreOneGroupAndASumOfNothingIsNull) {
  for (uint32_t chunk : {1u, 3u}) {
    SCOPED_TRACE(chunk);
    RowSource source({{std::nullopt, std::nullopt},
                      {std::nullopt, std::nullopt},
                      {1, std::nullopt},
                      {2, 0}},
                     chunk);
    Pipeline aggregated(source, CountAndSum(), {});
    Output out = Drain(aggregated);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_THAT(out.keys, ElementsAre(std::nullopt, 1, 2));
    EXPECT_THAT(out.counts, ElementsAre(2, 1, 1));
    EXPECT_THAT(out.sums, ElementsAre(std::nullopt, std::nullopt, 0));
  }
}

TEST(AggregateTest, WithoutKeysAllRowsAreOneGroup) {
  RowSource source({{1, 5}, {2, std::nullopt}, {3, 7}}, 2);
  Pipeline aggregated(source, CountAndSum(false), {});
  Output out = Drain(aggregated, false);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.counts, ElementsAre(3));
  EXPECT_THAT(out.sums, ElementsAre(12));
}

TEST(AggregateTest, NoRowsAreOneGroupWithoutKeysAndNoneWithThem) {
  RowSource source({}, 1);
  Pipeline all(source, CountAndSum(false), {});
  Output out = Drain(all, false);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.counts, ElementsAre(0));
  EXPECT_THAT(out.sums, ElementsAre(std::nullopt));

  Pipeline grouped(source, CountAndSum(), {});
  out = Drain(grouped);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.counts, IsEmpty());
}

TEST(AggregateTest, FoldsWhatGroupByGathersAndRunsAgain) {
  // Each group's rows are spread across the input, there are more groups
  // than fit in a batch, and groups straddle the batches GroupBy emits.
  constexpr uint32_t kGroups = kMaxBatchRows + 5;
  std::vector<std::pair<Cell, Cell>> rows;
  for (uint32_t i = 0; i < 3 * kGroups; ++i) {
    rows.push_back({i % kGroups, i / kGroups + 1});
  }
  RowSource source(std::move(rows), kMaxBatchRows, /*grouped=*/false);
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<GroupBy>(std::vector<uint32_t>{0}));
  for (auto& op : CountAndSum()) {
    ops.push_back(std::move(op));
  }
  Pipeline aggregated(source, std::move(ops), {});
  std::unique_ptr<OperatorState> state = aggregated.MakeState();
  for (int run = 0; run < 2; ++run) {
    SCOPED_TRACE(run);
    Output out = Drain(aggregated, *state);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    ASSERT_EQ(out.keys.size(), kGroups);
    for (uint32_t group = 0; group < kGroups; ++group) {
      ASSERT_EQ(out.keys[group], group);
      ASSERT_EQ(out.counts[group], 3);
      ASSERT_EQ(out.sums[group], 6);
    }
    aggregated.Rewind(*state);
  }
}

// Puts a name beside each (id, value) row of an ArraySource.
class Named final : public Operator {
 public:
  explicit Named(const std::vector<StringPool::Id>* names) : names_(names) {}
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<OperatorState>();
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override {
    out.Reset();
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      out.AddColumn(in.column(c), in.owner(c));
    }
    ColumnView names =
        ColumnView::Reference(StorageType{String{}}, names_->data());
    names.AdoptSelection(in.column(1));
    out.AddColumn(names);
    out.SetCardinality(in.size());
    return OpResult::kNeedMoreInput;
  }

 private:
  const std::vector<StringPool::Id>* names_;
};

TEST(AggregateTest, StringKeysGroupByTheirContents) {
  StringPool pool;
  std::vector<StringPool::Id> names = {
      pool.InternString("b"), pool.InternString("a"), pool.InternString("b"),
      StringPool::Id::Null(), pool.InternString("a"), pool.InternString("b")};
  test::ArraySource values({1, 2, 3, 4, 5, 6});
  AggregateSpec spec;
  spec.key_columns = {2};
  spec.group_column = 3;
  spec.aggregates = {{AggregateSpec::Function::kSum, 1}};
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<Named>(&names));
  ops.push_back(std::make_unique<GroupBy>(std::vector<uint32_t>{2}));
  ops.push_back(std::make_unique<Aggregate>(std::move(spec)));
  Pipeline aggregated(values, std::move(ops), {});
  std::unique_ptr<OperatorState> state = aggregated.MakeState();
  RowBatch batch;
  std::vector<std::string> keys;
  std::vector<int64_t> sums;
  while (aggregated.GetData(batch, *state)) {
    for (StringPool::Id id : test::ReadColumn<StringPool::Id>(batch, 0)) {
      keys.push_back(id.is_null() ? "NULL" : pool.Get(id).ToStdString());
    }
    for (int64_t sum : test::ReadColumn<int64_t>(batch, 1)) {
      sums.push_back(sum);
    }
  }
  ASSERT_TRUE(aggregated.status(*state).ok());
  EXPECT_THAT(keys, ElementsAre("b", "a", "NULL"));
  EXPECT_THAT(sums, ElementsAre(10, 7, 4));
}

TEST(AggregateTest, ASumWhichOverflowsFailsTheRun) {
  RowSource source({{1, std::numeric_limits<int64_t>::max()}, {1, 1}}, 2);
  Pipeline aggregated(source, CountAndSum(), {});
  Output out = Drain(aggregated);
  EXPECT_THAT(out.status.message(), HasSubstr("integer overflow"));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
