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

#include "src/trace_processor/core/exec/interval_intersect.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

using Row = std::vector<int64_t>;
using Table = std::vector<Row>;

// Emits a table of Int64 columns, a fixed number of rows at a time.
class TableSource final : public Source {
 public:
  TableSource(Table rows, uint32_t chunk_rows)
      : rows_(std::move(rows)),
        columns_(rows_.empty() ? 0
                               : static_cast<uint32_t>(rows_.front().size())),
        chunk_rows_(chunk_rows) {}

  uint32_t columns() const { return columns_; }

  std::unique_ptr<OperatorState> MakeState(Context& context) const override {
    auto state = std::make_unique<State>();
    state->context = &context;
    return state;
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto total = static_cast<uint32_t>(rows_.size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, total - s.offset);
    s.columns.assign(columns_, std::vector<int64_t>(count));
    for (uint32_t row = 0; row < count; ++row) {
      for (uint32_t c = 0; c < columns_; ++c) {
        s.columns[c][row] = rows_[s.offset + row][c];
      }
    }
    out.Reset();
    for (uint32_t c = 0; c < columns_; ++c) {
      test::AddCopy(*s.context, s.columns[c], nullptr, &out);
    }
    out.SetRowCount(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    Context* context = nullptr;
    ~State() override;
    uint32_t offset = 0;
    std::vector<std::vector<int64_t>> columns;
  };

  Table rows_;
  uint32_t columns_;
  uint32_t chunk_rows_;
};

TableSource::State::~State() = default;

// An operand whose ts and dur are its first two columns, the rest being
// whatever the table carries along. Every column is retained.
IntervalIntersectOperand Operand(const TableSource& source,
                                 std::vector<uint32_t> keys = {}) {
  IntervalIntersectOperand operand;
  operand.source = &source;
  operand.ts_column = 0;
  operand.dur_column = 1;
  operand.key_columns = std::move(keys);
  for (uint32_t c = 0; c < source.columns(); ++c) {
    operand.retained_columns.push_back(c);
  }
  return operand;
}

// Drains `intersect` into rows of `ts, dur` followed by every column of every
// operand, sorted so a test does not depend on the order keys are found in.
Table Collect(const IntervalIntersect& intersect,
              OperatorState& state,
              base::Status* status = nullptr) {
  Table rows;
  RowBatch batch;
  while (intersect.GetData(batch, state)) {
    for (uint32_t row = 0; row < batch.size(); ++row) {
      Row out;
      for (uint32_t c = 0; c < batch.column_count(); ++c) {
        out.push_back(batch.Value<int64_t>(c, row));
      }
      rows.push_back(std::move(out));
    }
  }
  if (status) {
    *status = intersect.status(state);
  } else {
    EXPECT_TRUE(intersect.status(state).ok())
        << intersect.status(state).message();
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

Table Intersect(std::vector<IntervalIntersectOperand> operands,
                base::Status* status = nullptr) {
  IntervalIntersect intersect(std::move(operands));
  std::unique_ptr<OperatorState> state =
      intersect.MakeState(test::TestContext());
  return Collect(intersect, *state, status);
}

TEST(IntervalIntersectTest, TwoOperandsMeetOverTheirSharedSpan) {
  // a: [0, 30)        [40, 50)
  // b:    [10, 20) [35, 45)
  TableSource a({{0, 30, 0}, {40, 10, 1}}, 8);
  TableSource b({{10, 10, 0}, {35, 10, 1}}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b)}),
              ElementsAre(Row{10, 10, 0, 30, 0, 10, 10, 0},
                          Row{40, 5, 40, 10, 1, 35, 10, 1}));
}

TEST(IntervalIntersectTest, MoreRegionsThanABatchHolds) {
  // Each operand is read in many chunks and the regions come back in more
  // batches than one.
  constexpr uint32_t kRows = kMaxBatchRows + 100;
  Table a;
  Table b;
  for (uint32_t i = 0; i < kRows; ++i) {
    a.push_back({i * 10, 10, i});
    b.push_back({i * 10 + 5, 10, i});
  }
  TableSource source_a(std::move(a), 64);
  TableSource source_b(std::move(b), 64);

  Table rows = Intersect({Operand(source_a), Operand(source_b)});
  // Every row of a but the last meets the row of b starting inside it, and
  // every row of b but the last meets the row of a which follows.
  ASSERT_EQ(rows.size(), 2 * kRows - 1);
  EXPECT_EQ(rows.front(), (Row{5, 5, 0, 10, 0, 5, 10, 0}));
  EXPECT_EQ(rows.back(), (Row{(kRows - 1) * 10 + 5, 5, (kRows - 1) * 10, 10,
                              kRows - 1, (kRows - 1) * 10 + 5, 10, kRows - 1}));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
