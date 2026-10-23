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

#include "src/trace_processor/core/exec/tree_from_intervals.h"

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

struct InputRow {
  int64_t ts;
  int64_t dur;
  int64_t key;
  std::optional<int64_t> next_key;
  int64_t value;
};

// Output row: (ts, dur, id, parent_id, key, value).
using OutRow = std::tuple<int64_t,
                          int64_t,
                          uint32_t,
                          std::optional<uint32_t>,
                          int64_t,
                          int64_t>;

// Input columns: (ts, dur, key, next_key, value, group).
TreeFromIntervalsSpec MakeSpec() {
  TreeFromIntervalsSpec spec;
  spec.input.ts_column = 0;
  spec.input.dur_column = 1;
  spec.input.key_columns = {2};
  spec.input.group_column = 5;
  spec.retained_columns = {2, 4};
  spec.next_key_columns = {3};

  spec.outputs = {
      {TreeFromIntervalsSpec::Source::kTs, 0},
      {TreeFromIntervalsSpec::Source::kDur, 0},
      {TreeFromIntervalsSpec::Source::kId, 0},
      {TreeFromIntervalsSpec::Source::kParentId, 0},
      {TreeFromIntervalsSpec::Source::kInput, 0},
      {TreeFromIntervalsSpec::Source::kInput, 1},
  };
  return spec;
}

std::vector<OutRow> RunTree(const TreeFromIntervals& op,
                            OperatorState& state,
                            const std::vector<InputRow>& input,
                            uint32_t chunk) {
  Context& context = test::TestContext();
  RowBatch in, out;
  std::vector<OutRow> rows;
  uint32_t groups = 0;
  std::optional<int64_t> last_key;
  auto collect = [&] {
    EXPECT_LE(out.size(), kMaxBatchRows);
    const ColumnView& pid_col = out.column(3);
    for (uint32_t r = 0; r < out.size(); ++r) {
      std::optional<uint32_t> pid;
      if (pid_col.IsValid(r)) {
        pid = out.Value<uint32_t>(3, r);
      }
      rows.emplace_back(out.Value<int64_t>(0, r), out.Value<int64_t>(1, r),
                        out.Value<uint32_t>(2, r), pid,
                        out.Value<int64_t>(4, r), out.Value<int64_t>(5, r));
    }
  };
  for (size_t at = 0; at < input.size(); at += chunk) {
    auto count =
        static_cast<uint32_t>(std::min<size_t>(chunk, input.size() - at));
    std::vector<int64_t> ts(count), dur(count), key(count), next_key(count),
        value(count);
    BitVector next_valid = BitVector::CreateWithSize(count);
    std::vector<uint32_t> group(count);
    for (uint32_t r = 0; r < count; ++r) {
      const InputRow& row = input[at + r];
      ts[r] = row.ts;
      dur[r] = row.dur;
      key[r] = row.key;
      next_valid.change(r, row.next_key.has_value());
      next_key[r] = row.next_key.value_or(0);
      value[r] = row.value;
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
    test::AddCopy(context, next_key, &next_valid, &in);
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
  return rows;
}

TEST(TreeFromIntervalsTest, CutsTransitiveChainAndOutputsParentFirstIds) {
  // Lane 1 [0, 30) -> Lane 2.
  // Lane 2 [0, 10) is a root (val 200); [10, 30) -> Lane 3.
  // Lane 3 [10, 20) -> Lane 4; [20, 30) is a root (val 301).
  // Lane 4 [10, 20) is a root (val 400).
  std::vector<InputRow> input = {
      {0, 30, 1, 2, 100},
      {0, 10, 2, std::nullopt, 200},
      {10, 20, 2, 3, 201},
      {10, 10, 3, 4, 300},
      {20, 10, 3, std::nullopt, 301},
      {10, 10, 4, std::nullopt, 400},
  };
  TreeFromIntervals op(MakeSpec());
  auto state = op.MakeState(test::TestContext());

  std::vector<OutRow> rows = RunTree(op, *state, input, 2);
  // Every parent must be emitted before its child (parent_id < id), and
  // following parent_id to root for Lane 1's slices must reach 200, 400, 301.
  for (size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(std::get<2>(rows[i]), static_cast<uint32_t>(i));
    if (std::optional<uint32_t> pid = std::get<3>(rows[i])) {
      EXPECT_LT(*pid, static_cast<uint32_t>(i));
    }
  }
  auto root_value = [&](size_t idx) {
    while (std::optional<uint32_t> pid = std::get<3>(rows[idx])) {
      idx = *pid;
    }
    return std::get<5>(rows[idx]);
  };
  std::vector<std::tuple<int64_t, int64_t, int64_t>> lane1_resolved;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (std::get<4>(rows[i]) == 1) {
      lane1_resolved.emplace_back(std::get<0>(rows[i]), std::get<1>(rows[i]),
                                  root_value(i));
    }
  }
  EXPECT_EQ(lane1_resolved, (std::vector<std::tuple<int64_t, int64_t, int64_t>>{
                                {0, 10, 200},
                                {10, 10, 400},
                                {20, 10, 301},
                            }));
}

TEST(TreeFromIntervalsTest, BreaksCyclesAndFallsBackOnTargetGaps) {
  std::vector<InputRow> input = {
      {0, 30, 1, 2, 10},
      {0, 10, 2, 1, 20},
      {20, 10, 2, 2, 21},
  };
  TreeFromIntervals op(MakeSpec());
  auto state = op.MakeState(test::TestContext());

  std::vector<OutRow> rows = RunTree(op, *state, input, 3);
  for (size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(std::get<2>(rows[i]), static_cast<uint32_t>(i));
    if (std::optional<uint32_t> pid = std::get<3>(rows[i])) {
      EXPECT_LT(*pid, static_cast<uint32_t>(i));
    }
  }
  auto root_value = [&](size_t idx) {
    while (std::optional<uint32_t> pid = std::get<3>(rows[idx])) {
      idx = *pid;
    }
    return std::get<5>(rows[idx]);
  };
  std::vector<std::tuple<int64_t, int64_t, int64_t>> lane1_resolved;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (std::get<4>(rows[i]) == 1) {
      lane1_resolved.emplace_back(std::get<0>(rows[i]), std::get<1>(rows[i]),
                                  root_value(i));
    }
  }
  EXPECT_EQ(lane1_resolved, (std::vector<std::tuple<int64_t, int64_t, int64_t>>{
                                {0, 10, 20},
                                {10, 10, 10},
                                {20, 10, 21},
                            }));
}

TEST(TreeFromIntervalsTest, ServesAcrossMultipleOutputBatches) {
  constexpr int64_t kSpans = kMaxBatchRows + 5;
  std::vector<InputRow> input;
  input.push_back({0, kSpans * 10, 1, 2, 1});
  for (int64_t i = 0; i < kSpans; ++i) {
    input.push_back({i * 10, 10, 2, std::nullopt, 1000 + i});
  }
  TreeFromIntervals op(MakeSpec());
  auto state = op.MakeState(test::TestContext());

  std::vector<OutRow> rows = RunTree(op, *state, input, kMaxBatchRows);
  EXPECT_EQ(rows.size(), static_cast<size_t>(2 * kSpans));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
