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

#include "src/trace_processor/core/exec/interval_fill_gaps.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Rows of Int64 columns, served in batches of at most kMaxBatchRows.
class RowsSource final : public Source {
 public:
  explicit RowsSource(std::vector<std::vector<int64_t>> rows)
      : rows_(std::move(rows)) {
    columns_.resize(rows_.empty() ? 0 : rows_[0].size());
    for (const auto& row : rows_) {
      for (size_t c = 0; c < row.size(); ++c) {
        columns_[c].push_back(row[c]);
      }
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
    auto total = static_cast<uint32_t>(rows_.size());
    if (s.emitted == total) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, total - s.emitted);
    out.Reset();
    for (const auto& column : columns_) {
      out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, column.data()));
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
  std::vector<std::vector<int64_t>> rows_;
  std::vector<std::vector<int64_t>> columns_;
};

// ts, dur, key, value, filled.
using Row = std::tuple<int64_t,
                       int64_t,
                       std::optional<int64_t>,
                       std::optional<int64_t>,
                       std::optional<int64_t>>;

// Input columns: ts, dur, key, value. Background columns: ts, dur, value,
// filled. Outputs: ts, dur, key, value (matched), filled (background only).
IntervalFillGapsSpec MakeSpec(const Source* background, bool keyed) {
  IntervalFillGapsSpec spec;
  spec.ts_column = 0;
  spec.dur_column = 1;
  if (keyed) {
    spec.key_columns = {2};
  }
  spec.background = background;
  spec.background_ts_column = 0;
  spec.background_dur_column = 1;
  spec.outputs = {{0, std::nullopt, StorageType{Int64{}}, "ts"},
                  {1, std::nullopt, StorageType{Int64{}}, "dur"},
                  {2, std::nullopt, StorageType{Int64{}}, "key"},
                  {3, 2, StorageType{Int64{}}, "value"},
                  {std::nullopt, 3, StorageType{Int64{}}, "filled"}};
  return spec;
}

// Runs `op` over `input` split into batches of `chunk` rows, refilling the
// same borrowed buffers for each, and returns every output row.
std::vector<Row> RunFill(
    const IntervalFillGaps& op,
    OperatorState& state,
    const std::vector<std::vector<std::optional<int64_t>>>& input,
    uint32_t chunk) {
  std::vector<int64_t> ts(chunk), dur(chunk), key(chunk), value(chunk);
  auto ts_valid = BitVector::CreateWithSize(chunk);
  RowBatch in, out;
  in.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, ts.data(), &ts_valid));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, key.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, value.data()));
  std::vector<Row> rows;
  auto collect = [&] {
    EXPECT_LE(out.size(), kMaxBatchRows);
    auto keys = test::ReadNullableColumn<int64_t>(out, 2);
    auto values = test::ReadNullableColumn<int64_t>(out, 3);
    auto filled = test::ReadNullableColumn<int64_t>(out, 4);
    auto starts = test::ReadNullableColumn<int64_t>(out, 0);
    for (uint32_t row = 0; row < out.size(); ++row) {
      rows.emplace_back(starts[row].value_or(-100),
                        out.column(1).Value<int64_t>(row), keys[row],
                        values[row], filled[row]);
    }
  };
  for (size_t at = 0; at < input.size(); at += chunk) {
    auto count =
        static_cast<uint32_t>(std::min<size_t>(chunk, input.size() - at));
    for (uint32_t row = 0; row < count; ++row) {
      const auto& r = input[at + row];
      ts_valid.change(row, r[0].has_value());
      ts[row] = r[0].value_or(0);
      dur[row] = *r[1];
      key[row] = *r[2];
      value[row] = *r[3];
    }
    in.SetCardinality(count);
    EXPECT_EQ(op.Execute(in, out, state), OpResult::kNeedMoreInput)
        << op.status(state).message();
    collect();
  }
  OpResult result;
  do {
    result = op.Finish(out, state);
    EXPECT_NE(result, OpResult::kError) << op.status(state).message();
    collect();
  } while (result == OpResult::kHaveMoreOutput);
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return std::tie(std::get<0>(a), std::get<2>(a), std::get<1>(a)) <
           std::tie(std::get<0>(b), std::get<2>(b), std::get<1>(b));
  });
  return rows;
}

TEST(IntervalFillGapsTest, FillsEveryLaneAcrossBatches) {
  // More gaps per lane than fit in one output batch, so Finish yields and
  // resumes; lane keys must survive the input buffers being refilled.
  constexpr int64_t kSpans = kMaxBatchRows + 3;
  RowsSource background({{0, 4 * kSpans, 7, 1}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/true));

  std::vector<std::vector<std::optional<int64_t>>> input;
  std::vector<Row> expected;
  for (int64_t lane : {10, 11}) {
    for (int64_t i = 0; i < kSpans; ++i) {
      input.push_back({4 * i, 1, lane, i});
      expected.emplace_back(4 * i, 1, lane, i, std::nullopt);
      expected.emplace_back(4 * i + 1, 3, lane, 7, 1);
    }
  }
  // A lane whose only row has no bounds covers nothing, so it is filled whole.
  input.push_back({std::nullopt, 5, 12, 0});
  expected.emplace_back(-100, 5, 12, 0, std::nullopt);
  expected.emplace_back(0, 4 * kSpans, 12, 7, 1);
  std::sort(expected.begin(), expected.end(), [](const Row& a, const Row& b) {
    return std::tie(std::get<0>(a), std::get<2>(a), std::get<1>(a)) <
           std::tie(std::get<0>(b), std::get<2>(b), std::get<1>(b));
  });

  auto state = op.MakeState();
  for (uint32_t chunk : {1u, 17u, kMaxBatchRows}) {
    SCOPED_TRACE(chunk);
    state->Reset();
    EXPECT_EQ(RunFill(op, *state, input, chunk), expected);
  }
}

TEST(IntervalFillGapsTest, OpenAndOverlappingInput) {
  RowsSource background({{0, 10, 7, 1}, {20, 10, 8, 1}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/false));
  auto state = op.MakeState();
  // Overlapping rows cover their union; a dur of -1 covers to the end.
  std::vector<Row> rows =
      RunFill(op, *state, {{2, 3, 0, 1}, {4, 3, 0, 2}, {25, -1, 0, 3}}, 4);
  // Without PER the key is the input's alone, so it is null on fillers.
  EXPECT_EQ(rows, (std::vector<Row>{{0, 2, std::nullopt, 7, 1},
                                    {2, 3, 0, 1, std::nullopt},
                                    {4, 3, 0, 2, std::nullopt},
                                    {7, 3, std::nullopt, 7, 1},
                                    {20, 5, std::nullopt, 8, 1},
                                    {25, -1, 0, 3, std::nullopt}}));
}

TEST(IntervalFillGapsTest, EmptyInputFillsTheBackground) {
  RowsSource background({{5, 10, 7, 1}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/false));
  auto state = op.MakeState();
  EXPECT_EQ(RunFill(op, *state, {}, 4),
            (std::vector<Row>{{5, 10, std::nullopt, 7, 1}}));
}

TEST(IntervalFillGapsTest, RefusesOverlappingBackground) {
  RowsSource background({{0, 10, 7, 1}, {5, 10, 8, 1}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/false));
  auto state = op.MakeState();
  RowBatch out;
  EXPECT_EQ(op.Finish(out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("overlap"));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
