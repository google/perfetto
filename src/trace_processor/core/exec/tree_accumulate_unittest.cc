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

#include "src/trace_processor/core/exec/tree_accumulate.h"

#include <cstdint>
#include <random>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Hands out a parent column and a value column in chunks of a fixed size,
// refilling one batch each time, which is what a real source does and what
// makes holding onto its chunk the wrong thing to do.
class ArraySource final : public Source {
 public:
  ArraySource(const std::vector<int64_t>* parent,
              const std::vector<int64_t>* value,
              uint32_t chunk_rows)
      : parent_(parent), value_(value), chunk_rows_(chunk_rows) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(parent_->size());
    if (s.offset == rows) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, rows - s.offset);
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, parent_->data()));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, value_->data()));
    out.Compose(RowSelection::Range(s.offset), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    uint32_t offset = 0;
  };

  const std::vector<int64_t>* parent_;
  const std::vector<int64_t>* value_;
  uint32_t chunk_rows_;
};

ArraySource::State::~State() = default;

// Drives a plan the way an executor does: it makes the state and owns the
// batch, so the plan itself stays const.
class Execution {
 public:
  explicit Execution(const Source& source)
      : source_(source), state_(source.MakeState()) {}

  RowBatch* Next() {
    return source_.GetData(batch_, *state_) ? &batch_ : nullptr;
  }
  void Rewind() { source_.Rewind(*state_); }
  base::Status status() const { return source_.status(*state_); }

 private:
  const Source& source_;
  std::unique_ptr<OperatorState> state_;
  RowBatch batch_;
};

// The definition, written out: a node's total is the sum over everything
// whose parent chain reaches it.
std::vector<int64_t> Reference(const std::vector<int64_t>& parent,
                               const std::vector<int64_t>& value) {
  std::vector<int64_t> totals(parent.size(), 0);
  for (size_t node = 0; node < parent.size(); ++node) {
    for (size_t row = 0; row < parent.size(); ++row) {
      for (int64_t walk = static_cast<int64_t>(row); walk >= 0;
           walk = parent[static_cast<size_t>(walk)]) {
        if (static_cast<size_t>(walk) == node) {
          totals[node] += value[row];
          break;
        }
      }
    }
  }
  return totals;
}

std::vector<int64_t> Totals(const std::vector<int64_t>& parent,
                            const std::vector<int64_t>& value,
                            uint32_t chunk_rows) {
  ArraySource source(&parent, &value, chunk_rows);
  TreeAccumulateUp accumulate(source, 0, 1);
  Execution run(accumulate);
  std::vector<int64_t> out;
  while (RowBatch* chunk = run.Next()) {
    // The added column is the last one; the ones it arrived with are still
    // there, in the order they were in.
    EXPECT_EQ(chunk->column_count(), 3u);
    // Read through the column's own row view rather than by position: a
    // column added on the way out need not sit in the same index space as
    // the ones it was added beside, and reading it by position is how that
    // going wrong stays invisible.
    const ColumnView& column = chunk->column(2);
    const auto* totals = static_cast<const int64_t*>(column.data());
    for (uint32_t row = 0; row < chunk->size(); ++row) {
      out.push_back(totals[column.selection().GetIndex(row)]);
    }
  }
  EXPECT_TRUE(run.status().ok()) << run.status().message();
  return out;
}

// A source which materialises its rows refills the same buffers on every
// pull, so what a held view points at is whatever it wrote next. Holding on
// to a chunk from one of these is the case a view cannot serve.
class RefillingSource final : public Source {
 public:
  RefillingSource(const std::vector<int64_t>* parent,
                  const std::vector<int64_t>* value,
                  uint32_t chunk_rows)
      : parent_(parent), value_(value), chunk_rows_(chunk_rows) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(parent_->size());
    if (s.offset == rows) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, rows - s.offset);
    s.parent_buffer.assign(parent_->begin() + s.offset,
                           parent_->begin() + s.offset + count);
    s.value_buffer.assign(value_->begin() + s.offset,
                          value_->begin() + s.offset + count);
    out.Reset();
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, s.parent_buffer.data()));
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, s.value_buffer.data()));
    out.Compose(RowSelection::Range(0), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    uint32_t offset = 0;
    std::vector<int64_t> parent_buffer;
    std::vector<int64_t> value_buffer;
  };

  const std::vector<int64_t>* parent_;
  const std::vector<int64_t>* value_;
  uint32_t chunk_rows_;
};

RefillingSource::State::~State() = default;

// Parents come before their children, which is what the stage before this
// one is for.
std::vector<int64_t> RandomParents(std::mt19937& rng, uint32_t rows) {
  std::vector<int64_t> parent(rows, -1);
  for (uint32_t i = 1; i < rows; ++i) {
    if (std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
      continue;
    }
    parent[i] = std::uniform_int_distribution<int64_t>(0, i - 1)(rng);
  }
  return parent;
}

TEST(TreeAccumulateUpChunkTest, MatchesTheDefinition) {
  std::mt19937 rng(0xfeed);
  for (int trial = 0; trial < 50; ++trial) {
    auto rows = std::uniform_int_distribution<uint32_t>(0, 60)(rng);
    std::vector<int64_t> parent = RandomParents(rng, rows);
    std::vector<int64_t> value(rows);
    for (uint32_t i = 0; i < rows; ++i) {
      value[i] = std::uniform_int_distribution<int64_t>(-100, 100)(rng);
    }
    EXPECT_EQ(Totals(parent, value, 8), Reference(parent, value))
        << "trial " << trial;
  }
}

// A subtree spans whatever chunks it happens to span, so the answer cannot
// depend on where the boundaries fall.
TEST(TreeAccumulateUpChunkTest, TheChunkSizeDoesNotChangeTheAnswer) {
  std::mt19937 rng(7);
  std::vector<int64_t> parent = RandomParents(rng, 64);
  std::vector<int64_t> value(64);
  for (uint32_t i = 0; i < 64; ++i) {
    value[i] = i + 1;
  }
  std::vector<int64_t> whole = Totals(parent, value, 1024);
  for (uint32_t chunk_rows : {1u, 2u, 7u, 63u, 64u, 65u}) {
    EXPECT_EQ(Totals(parent, value, chunk_rows), whole)
        << "chunk size " << chunk_rows;
  }
}

// Running again refills the buffers the store already had rather than asking
// for new ones, which is what makes a store the whole of what is held: the
// second run costs the same memory as the first, not twice.
TEST(TreeAccumulateUpChunkTest, RunningAgainRefillsTheSameStore) {
  std::mt19937 rng(11);
  std::vector<int64_t> parent = RandomParents(rng, 100);
  std::vector<int64_t> value(100, 1);
  ArraySource source(&parent, &value, 10);
  TreeAccumulateUp accumulate(source, 0, 1);

  Execution run(accumulate);
  RowBatch* first = run.Next();
  ASSERT_NE(first, nullptr);
  const void* payload = first->column(1).data();
  while (run.Next()) {
  }

  run.Rewind();
  RowBatch* again = run.Next();
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again->column(1).data(), payload);
}

TEST(TreeAccumulateUpChunkTest, AnEmptyInputProducesNothing) {
  std::vector<int64_t> parent;
  std::vector<int64_t> value;
  EXPECT_TRUE(Totals(parent, value, 8).empty());
}

// The answer must not depend on whether the source's storage stands still.
TEST(TreeAccumulateUpTest, ASourceWhichRefillsItsBuffersGivesTheSameAnswer) {
  std::mt19937 rng(7);
  std::vector<int64_t> parent = RandomParents(rng, 200);
  std::vector<int64_t> value(parent.size());
  for (uint32_t i = 0; i < value.size(); ++i) {
    value[i] = std::uniform_int_distribution<int64_t>(-50, 50)(rng);
  }

  std::vector<int64_t> expected = Totals(parent, value, 16);

  RefillingSource source(&parent, &value, 16);
  TreeAccumulateUp accumulate(source, 0, 1);
  Execution run(accumulate);
  std::vector<int64_t> got;
  std::vector<int64_t> payload;
  while (RowBatch* chunk = run.Next()) {
    const ColumnView& totals_column = chunk->column(2);
    const ColumnView& value_column = chunk->column(1);
    const auto* totals = static_cast<const int64_t*>(totals_column.data());
    const auto* values = static_cast<const int64_t*>(value_column.data());
    for (uint32_t row = 0; row < chunk->size(); ++row) {
      got.push_back(totals[totals_column.selection().GetIndex(row)]);
      payload.push_back(values[value_column.selection().GetIndex(row)]);
    }
  }
  ASSERT_TRUE(run.status().ok()) << run.status().message();
  EXPECT_EQ(got, Reference(parent, value));
  EXPECT_EQ(got, expected);
  EXPECT_EQ(payload, value);
}

// The payload has to come back out beside the total, at the same rows.
TEST(TreeAccumulateUpTest, ThePayloadComesBackOutBesideTheTotal) {
  std::vector<int64_t> parent = {-1, 0, 0, 1, 1, 2, 2};
  std::vector<int64_t> value = {1, 2, 3, 4, 5, 6, 7};

  RefillingSource source(&parent, &value, 2);
  TreeAccumulateUp accumulate(source, 0, 1);

  std::vector<int64_t> values_out;
  std::vector<int64_t> totals_out;
  RowCursor cursor(accumulate);
  for (bool more = cursor.Open(); more; more = cursor.Next()) {
    const RowBatch& chunk = cursor.batch();
    values_out.push_back(
        static_cast<const int64_t*>(chunk.column(1).data())[cursor.row(1)]);
    totals_out.push_back(
        static_cast<const int64_t*>(chunk.column(2).data())[cursor.row(2)]);
  }
  EXPECT_EQ(values_out, value);
  EXPECT_EQ(totals_out, Reference(parent, value));
}

// What the split buys: the plan is const and holds nothing, so two runs of it
// can be in flight at the same time without knowing about each other.
TEST(TreeAccumulateUpChunkTest, OnePlanRunsTwiceAtOnce) {
  std::mt19937 rng(5);
  std::vector<int64_t> parent = RandomParents(rng, 300);
  std::vector<int64_t> value(parent.size());
  for (uint32_t i = 0; i < value.size(); ++i) {
    value[i] = std::uniform_int_distribution<int64_t>(-20, 20)(rng);
  }
  std::vector<int64_t> want = Reference(parent, value);

  ArraySource source(&parent, &value, 32);
  const TreeAccumulateUp accumulate(source, 0, 1);

  // Two executions, stepped alternately down to the batch.
  Execution first(accumulate);
  Execution second(accumulate);
  std::vector<int64_t> got_first;
  std::vector<int64_t> got_second;
  for (;;) {
    RowBatch* a = first.Next();
    RowBatch* b = second.Next();
    if (!a && !b) {
      break;
    }
    ASSERT_EQ(a != nullptr, b != nullptr);
    for (uint32_t row = 0; row < a->size(); ++row) {
      const ColumnView& ca = a->column(2);
      const ColumnView& cb = b->column(2);
      got_first.push_back(
          static_cast<const int64_t*>(ca.data())[ca.selection().GetIndex(row)]);
      got_second.push_back(
          static_cast<const int64_t*>(cb.data())[cb.selection().GetIndex(row)]);
    }
  }
  EXPECT_EQ(got_first, want);
  EXPECT_EQ(got_second, want);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
