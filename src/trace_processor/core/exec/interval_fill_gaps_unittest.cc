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
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Rows of Int64 columns, served in batches of at most kMaxBatchRows.
class RowsSource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit RowsSource(std::vector<std::vector<int64_t>> rows)
      : rows_(std::move(rows)) {
    columns_.resize(rows_.empty() ? 0 : rows_[0].size());
    for (const auto& row : rows_) {
      for (size_t c = 0; c < row.size(); ++c) {
        columns_[c].push_back(row[c]);
      }
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
    for (const auto& column : columns_) {
      out.AddBorrowedColumn(ColumnView::Reference(
          StorageType{Int64{}}, column.data(), nullptr, s.emitted));
    }
    out.SetRowCount(count);
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

// ts, dur, key, value.
using Row = std::
    tuple<int64_t, int64_t, std::optional<int64_t>, std::optional<int64_t>>;

// Input columns: ts, dur, key, value, group number. Background columns: ts,
// dur, value.
IntervalFillGapsSpec MakeSpec(const Source* background, bool keyed) {
  IntervalFillGapsSpec spec;
  spec.input.ts_column = 0;
  spec.input.dur_column = 1;
  spec.input.forever = true;
  if (keyed) {
    spec.input.key_columns = {2};
    spec.input.group_column = 4;
  }
  spec.background.source = background;
  spec.background.lanes.ts_column = 0;
  spec.background.lanes.dur_column = 1;
  spec.background.retained_columns = {2};
  constexpr uint32_t kNone = IntervalFillGapsSpec::Output::kNone;
  spec.outputs = {{0, kNone, StorageType{Int64{}}, "ts"},
                  {1, kNone, StorageType{Int64{}}, "dur"},
                  {2, kNone, StorageType{Int64{}}, "key"},
                  {3, 0, StorageType{Int64{}}, "value"}};
  return spec;
}

// Runs `op` over `input` split into batches of `chunk` rows, and returns every
// output row. Each batch is copied into its own buffers. The input must be
// grouped by key and sorted by ts within each key.
std::vector<Row> RunFill(
    const IntervalFillGaps& op,
    OperatorState& state,
    const std::vector<std::vector<std::optional<int64_t>>>& input,
    uint32_t chunk) {
  Context& context = test::TestContext();
  RowBatch in, out;
  std::vector<Row> rows;
  uint32_t groups = 0;
  std::optional<int64_t> last_key;
  auto collect = [&] {
    EXPECT_LE(out.size(), kMaxBatchRows);
    auto keys = test::ReadNullableColumn<int64_t>(out, 2);
    auto values = test::ReadNullableColumn<int64_t>(out, 3);
    auto starts = test::ReadNullableColumn<int64_t>(out, 0);
    for (uint32_t row = 0; row < out.size(); ++row) {
      rows.emplace_back(starts[row].value_or(-100), out.Value<int64_t>(1, row),
                        keys[row], values[row]);
    }
  };
  for (size_t at = 0; at < input.size(); at += chunk) {
    auto count =
        static_cast<uint32_t>(std::min<size_t>(chunk, input.size() - at));
    std::vector<int64_t> ts(count), dur(count), key(count), value(count);
    std::vector<uint32_t> group(count);
    auto ts_valid = BitVector::CreateWithSize(count);
    for (uint32_t row = 0; row < count; ++row) {
      const auto& r = input[at + row];
      ts_valid.change(row, r[0].has_value());
      ts[row] = r[0].value_or(0);
      dur[row] = *r[1];
      key[row] = *r[2];
      value[row] = *r[3];
      if (key[row] != last_key) {
        last_key = key[row];
        ++groups;
      }
      group[row] = groups - 1;
    }
    in.Reset();
    test::AddCopy(context, ts, &ts_valid, &in);
    test::AddCopy(context, dur, nullptr, &in);
    test::AddCopy(context, key, nullptr, &in);
    test::AddCopy(context, value, nullptr, &in);
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
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return std::tie(std::get<0>(a), std::get<2>(a), std::get<1>(a)) <
           std::tie(std::get<0>(b), std::get<2>(b), std::get<1>(b));
  });
  return rows;
}

TEST(IntervalFillGapsTest, FillsEveryLaneAcrossBatches) {
  // More gaps per lane than fit in one output batch, so the operator has to
  // output them over several calls. Lane keys must survive the input buffers
  // being refilled.
  constexpr int64_t kSpans = kMaxBatchRows + 3;
  RowsSource background({{0, 4 * kSpans, 7}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/true));

  std::vector<std::vector<std::optional<int64_t>>> input;
  std::vector<Row> expected;
  for (int64_t lane : {10, 11}) {
    for (int64_t i = 0; i < kSpans; ++i) {
      input.push_back({4 * i, 1, lane, i});
      expected.emplace_back(4 * i, 1, lane, i);
      expected.emplace_back(4 * i + 1, 3, lane, 7);
    }
  }
  // A lane whose only row has no bounds covers nothing, so it is filled whole.
  input.push_back({std::nullopt, 5, 12, 0});
  expected.emplace_back(-100, 5, 12, 0);
  expected.emplace_back(0, 4 * kSpans, 12, 7);
  std::sort(expected.begin(), expected.end(), [](const Row& a, const Row& b) {
    return std::tie(std::get<0>(a), std::get<2>(a), std::get<1>(a)) <
           std::tie(std::get<0>(b), std::get<2>(b), std::get<1>(b));
  });

  auto state = op.MakeState(test::TestContext());
  for (uint32_t chunk : {1u, 17u, kMaxBatchRows}) {
    SCOPED_TRACE(chunk);
    state->EndRun();
    EXPECT_EQ(RunFill(op, *state, input, chunk), expected);
  }
}

TEST(IntervalFillGapsTest, OverlappingRowsCoverTheirUnion) {
  RowsSource background({{0, 100, 7}});
  IntervalFillGaps op(MakeSpec(&background, /*keyed=*/false));
  auto state = op.MakeState(test::TestContext());
  // Some rows overlap and some touch. Together they cover [10, 30) and
  // [50, 60).
  std::vector<Row> rows = RunFill(
      op, *state,
      {{10, 10, 0, 3}, {12, 3, 0, 4}, {20, 10, 0, 2}, {50, 10, 0, 1}}, 2);
  EXPECT_EQ(rows, (std::vector<Row>{{0, 10, std::nullopt, 7},
                                    {10, 10, 0, 3},
                                    {12, 3, 0, 4},
                                    {20, 10, 0, 2},
                                    {30, 20, std::nullopt, 7},
                                    {50, 10, 0, 1},
                                    {60, 40, std::nullopt, 7}}));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
