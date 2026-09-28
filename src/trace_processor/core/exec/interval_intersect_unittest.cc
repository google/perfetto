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
#include "src/trace_processor/core/exec/row_selection.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

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

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
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
      out.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, s.columns[c].data()));
    }
    out.Compose(RowSelection::Range(0), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
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
// whatever the table carries along.
IntervalIntersectOperand Operand(const Source& source,
                                 std::vector<uint32_t> keys = {}) {
  IntervalIntersectOperand operand;
  operand.source = &source;
  operand.ts_column = 0;
  operand.dur_column = 1;
  operand.key_columns = std::move(keys);
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
        out.push_back(batch.column(c).Value<int64_t>(row));
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
  std::unique_ptr<OperatorState> state = intersect.MakeState();
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

TEST(IntervalIntersectTest, TouchingEndToStartIsNoMeeting) {
  TableSource a({{0, 10, 0}}, 8);
  TableSource b({{10, 10, 0}}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b)}), IsEmpty());
}

TEST(IntervalIntersectTest, PointMeetsTheInstantsAnIntervalCovers) {
  // The interval [10, 20) covers the instants 10 through 19, and its end is
  // not one of them.
  TableSource points({{5, 0, 0}, {10, 0, 1}, {19, 0, 2}, {20, 0, 3}}, 8);
  TableSource interval({{10, 10, 0}}, 8);

  EXPECT_THAT(Intersect({Operand(points), Operand(interval)}),
              ElementsAre(Row{10, 0, 10, 0, 1, 10, 10, 0},
                          Row{19, 0, 19, 0, 2, 10, 10, 0}));
}

TEST(IntervalIntersectTest, PointsMeetOnlyAtTheSameInstant) {
  TableSource a({{10, 0, 0}, {12, 0, 1}}, 8);
  TableSource b({{10, 0, 0}, {11, 0, 1}}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b)}),
              ElementsAre(Row{10, 0, 10, 0, 0, 10, 0, 0}));
}

TEST(IntervalIntersectTest, NarrowingThreeOperandsCanLeaveAPoint) {
  // [0, 100) narrowed by [20, 30) and then by the point 25.
  TableSource a({{0, 100, 0}}, 8);
  TableSource b({{20, 10, 0}}, 8);
  TableSource c({{25, 0, 0}}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b), Operand(c)}),
              ElementsAre(Row{25, 0, 0, 100, 0, 20, 10, 0, 25, 0, 0}));
}

TEST(IntervalIntersectTest, AnOperandCoveringNothingLeavesNothing) {
  TableSource a({{0, 100, 0}}, 8);
  TableSource b({{10, 5, 0}}, 8);
  TableSource c({{20, 0, 0}}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b), Operand(c)}), IsEmpty());
}

TEST(IntervalIntersectTest, KeysConfineRegionsToRowsWhichAgreeOnThem) {
  // Column 2 is the key: only key 2 is in both operands.
  TableSource a({{10, 0, 1, 0}, {10, 0, 2, 1}}, 8);
  TableSource b({{0, 20, 2, 0}, {0, 20, 3, 1}}, 8);

  EXPECT_THAT(Intersect({Operand(a, {2}), Operand(b, {2})}),
              ElementsAre(Row{10, 0, 10, 0, 2, 1, 0, 20, 2, 0}));
}

TEST(IntervalIntersectTest, RowsAreKeyedOnEveryKeyColumn) {
  TableSource a({{0, 10, 1, 1}, {0, 10, 1, 2}}, 8);
  TableSource b({{5, 10, 1, 2}}, 8);

  EXPECT_THAT(Intersect({Operand(a, {2, 3}), Operand(b, {2, 3})}),
              ElementsAre(Row{5, 5, 0, 10, 1, 2, 5, 10, 1, 2}));
}

TEST(IntervalIntersectTest, AnEmptyOperandLeavesNothing) {
  TableSource a({{0, 100, 0}}, 8);
  TableSource b({}, 8);

  EXPECT_THAT(Intersect({Operand(a), Operand(b)}), IsEmpty());
}

TEST(IntervalIntersectTest, ARowOfOverlappingIntervalsIsSearchedCorrectly) {
  // An overlapping operand takes a different path through the intersector
  // than one whose rows are laid end to end.
  TableSource points({{20, 0, 0}}, 8);
  TableSource intervals({{0, 30, 0}, {5, 20, 1}, {10, 2, 2}, {18, 10, 3}}, 8);

  EXPECT_THAT(Intersect({Operand(points), Operand(intervals)}),
              ElementsAre(Row{20, 0, 20, 0, 0, 0, 30, 0},
                          Row{20, 0, 20, 0, 0, 5, 20, 1},
                          Row{20, 0, 20, 0, 0, 18, 10, 3}));
}

TEST(IntervalIntersectTest, DurationBelowZeroIsRefused) {
  TableSource a({{0, 10, 0}}, 8);
  TableSource b({{500, -1, 0}}, 8);

  base::Status status = base::OkStatus();
  EXPECT_THAT(Intersect({Operand(a), Operand(b)}, &status), IsEmpty());
  EXPECT_FALSE(status.ok());
}

TEST(IntervalIntersectTest, TimestampBelowZeroIsRefused) {
  TableSource a({{-1, 10, 0}}, 8);
  TableSource b({{0, 10, 0}}, 8);

  base::Status status = base::OkStatus();
  EXPECT_THAT(Intersect({Operand(a), Operand(b)}, &status), IsEmpty());
  EXPECT_FALSE(status.ok());
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

TEST(IntervalIntersectTest, RewindingGivesTheSameRegions) {
  TableSource a({{0, 30, 0}, {40, 10, 1}}, 4);
  TableSource b({{10, 10, 0}, {35, 10, 1}}, 4);

  IntervalIntersect intersect({Operand(a), Operand(b)});
  std::unique_ptr<OperatorState> state = intersect.MakeState();
  Table first = Collect(intersect, *state);
  intersect.Rewind(*state);
  EXPECT_EQ(Collect(intersect, *state), first);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
