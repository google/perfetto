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

#include "src/trace_processor/core/exec/interval_walk.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

struct EdgeRow {
  int64_t ts;
  int64_t dur;
  int64_t key;
  std::optional<int64_t> next_key;
  int64_t value;
};

// Edges source: columns are (ts, dur, key, next_key, value, group).
class EdgesSource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit EdgesSource(std::vector<EdgeRow> rows) : rows_(std::move(rows)) {
    uint32_t groups = 0;
    std::optional<int64_t> last_key;
    next_valid_ =
        BitVector::CreateWithSize(static_cast<uint32_t>(rows_.size()));
    for (size_t i = 0; i < rows_.size(); ++i) {
      const EdgeRow& r = rows_[i];
      ts_.push_back(r.ts);
      dur_.push_back(r.dur);
      key_.push_back(r.key);
      next_valid_.change(static_cast<uint32_t>(i), r.next_key.has_value());
      next_key_.push_back(r.next_key.value_or(0));
      value_.push_back(r.value);
      if (r.key != last_key) {
        last_key = r.key;
        ++groups;
      }
      group_.push_back(groups - 1);
    }
  }

  std::unique_ptr<OperatorState> MakeState(Context&) const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto total = static_cast<uint32_t>(rows_.size());
    if (s.emitted == total) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, total - s.emitted);
    out.Reset();
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, ts_.data(), nullptr, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, dur_.data(), nullptr, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, key_.data(), nullptr, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, next_key_.data(), &next_valid_, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Int64{}}, value_.data(), nullptr, s.emitted));
    out.AddBorrowedColumn(ColumnView::Reference(
        StorageType{Uint32{}}, group_.data(), nullptr, s.emitted));
    out.SetRowCount(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };
  std::vector<EdgeRow> rows_;
  std::vector<int64_t> ts_;
  std::vector<int64_t> dur_;
  std::vector<int64_t> key_;
  BitVector next_valid_;
  std::vector<int64_t> next_key_;
  std::vector<int64_t> value_;
  std::vector<uint32_t> group_;
};

// Output row: (ts, dur, root_key, leaf_key, leaf_value).
using OutRow = std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t>;

// Input columns: (ts, dur, key, group).
// Edge columns: (ts, dur, key, next_key, value, group).
IntervalWalkSpec MakeSpec(const Source* edges) {
  IntervalWalkSpec spec;
  spec.input.ts_column = 0;
  spec.input.dur_column = 1;
  spec.input.key_columns = {2};
  spec.input.group_column = 3;
  spec.input_retained_columns = {2};

  spec.edges.source = edges;
  spec.edges.lanes.ts_column = 0;
  spec.edges.lanes.dur_column = 1;
  spec.edges.lanes.key_columns = {2};
  spec.edges.lanes.group_column = 5;
  spec.edges.retained_columns = {2, 3, 4};
  spec.next_key_columns = {1};

  spec.outputs = {
      {IntervalWalkSpec::Source::kTs, 0},
      {IntervalWalkSpec::Source::kDur, 0},
      {IntervalWalkSpec::Source::kInput, 0},
      {IntervalWalkSpec::Source::kEdge, 0},
      {IntervalWalkSpec::Source::kEdge, 2},
  };
  return spec;
}

std::vector<OutRow> RunWalk(
    const IntervalWalk& op,
    OperatorState& state,
    const std::vector<std::tuple<int64_t, int64_t, int64_t>>& input,
    uint32_t chunk) {
  Context& context = test::TestContext();
  RowBatch in, out;
  std::vector<OutRow> rows;
  uint32_t groups = 0;
  std::optional<int64_t> last_key;
  auto collect = [&] {
    EXPECT_LE(out.size(), kMaxBatchRows);
    for (uint32_t r = 0; r < out.size(); ++r) {
      rows.emplace_back(out.Value<int64_t>(0, r), out.Value<int64_t>(1, r),
                        out.Value<int64_t>(2, r), out.Value<int64_t>(3, r),
                        out.Value<int64_t>(4, r));
    }
  };
  for (size_t at = 0; at < input.size(); at += chunk) {
    auto count =
        static_cast<uint32_t>(std::min<size_t>(chunk, input.size() - at));
    std::vector<int64_t> ts(count), dur(count), key(count);
    std::vector<uint32_t> group(count);
    for (uint32_t r = 0; r < count; ++r) {
      std::tie(ts[r], dur[r], key[r]) = input[at + r];
      if (key[r] != last_key) {
        last_key = key[r];
        ++groups;
      }
      group[r] = groups - 1;
    }
    in.Reset();
    test::AddCopy(context, ts, nullptr, &in);
    test::AddCopy(context, dur, nullptr, &in);
    test::AddCopy(context, key, nullptr, &in);
    test::AddCopy(context, group, nullptr, &in);
    in.SetRowCount(count);
    OpResult result;
    do {
      result = op.Execute(in, out, state);
      EXPECT_NE(result, OpResult::kError) << op.status(state).message();
      collect();
    } while (result == OpResult::kHaveMoreOutput);
  }
  OpResult result;
  do {
    result = op.Finish(out, state);
    EXPECT_NE(result, OpResult::kError) << op.status(state).message();
    collect();
  } while (result == OpResult::kHaveMoreOutput);
  return rows;
}

TEST(IntervalWalkTest, WalksTransitiveChainAndSplitsIntervals) {
  // Lane 1 [0, 30) delegates to Lane 2.
  // Lane 2 [0, 10) is a leaf (value 200); [10, 30) delegates to Lane 3.
  // Lane 3 [10, 20) delegates to Lane 4; [20, 30) is a leaf (value 300).
  // Lane 4 [10, 20) is a leaf (value 400).
  EdgesSource edges({
      {0, 30, 1, 2, 100},
      {0, 10, 2, std::nullopt, 200},
      {10, 20, 2, 3, 201},
      {10, 10, 3, 4, 300},
      {20, 10, 3, std::nullopt, 301},
      {10, 10, 4, std::nullopt, 400},
  });
  IntervalWalk op(MakeSpec(&edges));
  auto state = op.MakeState(test::TestContext());

  std::vector<OutRow> rows = RunWalk(op, *state, {{0, 30, 1}}, 1);
  EXPECT_EQ(rows, (std::vector<OutRow>{
                      {0, 10, 1, 2, 200},
                      {10, 10, 1, 4, 400},
                      {20, 10, 1, 3, 301},
                  }));
}

TEST(IntervalWalkTest, BreaksCyclesAndFallsBackOnTargetGaps) {
  // Lane 1 [0, 30) -> Lane 2 (value 10).
  // Lane 2 [0, 10) -> Lane 1 (cycle! stops at Lane 2, value 20).
  // Lane 2 [10, 20) has a gap (falls back to Lane 1's edge, value 10).
  // Lane 2 [20, 30) -> Lane 2 (self-loop! stops at Lane 2, value 21).
  EdgesSource edges({
      {0, 30, 1, 2, 10},
      {0, 10, 2, 1, 20},
      {20, 10, 2, 2, 21},
  });
  IntervalWalk op(MakeSpec(&edges));
  auto state = op.MakeState(test::TestContext());

  std::vector<OutRow> rows = RunWalk(op, *state, {{0, 30, 1}}, 1);
  EXPECT_EQ(rows, (std::vector<OutRow>{
                      {0, 10, 1, 2, 20},
                      {10, 10, 1, 1, 10},
                      {20, 10, 1, 2, 21},
                  }));
}

TEST(IntervalWalkTest, ServesAcrossMultipleOutputBatches) {
  constexpr int64_t kSpans = kMaxBatchRows + 5;
  std::vector<EdgeRow> edge_rows;
  edge_rows.push_back({0, kSpans * 2, 1, 2, 10});
  std::vector<OutRow> expected;
  for (int64_t i = 0; i < kSpans; ++i) {
    edge_rows.push_back({i * 2, 2, 2, std::nullopt, 100 + i});
    expected.emplace_back(i * 2, 2, 1, 2, 100 + i);
  }
  EdgesSource edges(std::move(edge_rows));
  IntervalWalk op(MakeSpec(&edges));
  auto state = op.MakeState(test::TestContext());

  EXPECT_EQ(RunWalk(op, *state, {{0, kSpans * 2, 1}}, 1), expected);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
