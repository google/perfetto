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

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

AggregateCall Sum(uint32_t column) {
  return {AggregateCall::Function::kSum, column};
}

// Emits id, parent id and a value, in whatever order the rows were given.
class RowSource final : public Source {
 public:
  // `order` is the order the rows are emitted in, which is not the order they
  // are numbered in.
  RowSource(std::vector<int64_t> parents,
            std::vector<int64_t> values,
            uint32_t chunk_rows,
            std::vector<uint32_t> order = {})
      : parents_(std::move(parents)),
        values_(std::move(values)),
        chunk_rows_(chunk_rows),
        order_(std::move(order)) {
    if (order_.empty()) {
      for (uint32_t i = 0; i < parents_.size(); ++i) {
        order_.push_back(i);
      }
    }
  }

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
    auto total = static_cast<uint32_t>(parents_.size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, total - s.offset);
    s.ids.resize(count);
    s.parents.resize(count);
    s.values.resize(count);
    s.validity = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t row = order_[s.offset + i];
      s.ids[i] = row;
      s.values[i] = values_[row];
      s.parents[i] = parents_[row] < 0 ? 0 : parents_[row];
      if (parents_[row] >= 0) {
        s.validity.set(i);
      }
    }
    out.Reset();
    test::AddCopy(*s.context, s.ids, nullptr, &out);
    test::AddCopy(*s.context, s.parents, &s.validity, &out);
    test::AddCopy(*s.context, s.values, nullptr, &out);
    out.SetRowCount(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    Context* context = nullptr;
    uint32_t offset = 0;
    std::vector<int64_t> ids;
    std::vector<int64_t> parents;
    std::vector<int64_t> values;
    BitVector validity;
  };

  std::vector<int64_t> parents_;
  std::vector<int64_t> values_;
  uint32_t chunk_rows_;
  std::vector<uint32_t> order_;
};

RowSource::State::~State() = default;

// The two folds written out directly: up sums everything below a node, down
// sums everything above it.
std::vector<int64_t> ReferenceUp(const std::vector<int64_t>& parent,
                                 const std::vector<int64_t>& value) {
  std::vector<int64_t> totals(parent.size(), 0);
  for (size_t row = 0; row < parent.size(); ++row) {
    for (int64_t walk = static_cast<int64_t>(row); walk >= 0;
         walk = parent[static_cast<size_t>(walk)]) {
      totals[static_cast<size_t>(walk)] += value[row];
    }
  }
  return totals;
}

std::vector<int64_t> ReferenceDown(const std::vector<int64_t>& parent,
                                   const std::vector<int64_t>& value) {
  std::vector<int64_t> totals(parent.size(), 0);
  for (size_t row = 0; row < parent.size(); ++row) {
    for (int64_t walk = static_cast<int64_t>(row); walk >= 0;
         walk = parent[static_cast<size_t>(walk)]) {
      totals[row] += value[static_cast<size_t>(walk)];
    }
  }
  return totals;
}

// Runs the whole pipeline, returning the totals by id and the number of
// batches produced.
struct Result {
  std::vector<int64_t> totals;
  uint32_t batches = 0;
};

Result Accumulate(const std::vector<int64_t>& parent,
                  const std::vector<int64_t>& value,
                  uint32_t chunk_rows,
                  bool up,
                  std::vector<uint32_t> order = {},
                  bool already_ordered = false) {
  RowSource source(parent, value, chunk_rows, std::move(order));
  // Ids become node numbers before anything else sees them.
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  if (!already_ordered) {
    if (up) {
      ops.push_back(std::make_unique<TreeChildFirst>(3, 4));
    } else {
      ops.push_back(std::make_unique<TreeParentFirst>(3, 4));
    }
  }
  TreeAccumulateSpec spec{3, 4, {Sum(2)}};
  if (up) {
    ops.push_back(std::make_unique<TreeAccumulateUp>(spec));
  } else {
    ops.push_back(std::make_unique<TreeAccumulateDown>(spec));
  }
  Pipeline pipeline(source, std::move(ops), {});

  std::unique_ptr<OperatorState> state =
      pipeline.MakeState(test::TestContext());
  RowBatch batch;
  Result result;
  result.totals.assign(parent.size(), 0);
  while (pipeline.GetData(batch, *state)) {
    ++result.batches;
    std::vector<int64_t> ids = test::ReadColumn<int64_t>(batch, 0);
    std::vector<int64_t> totals = test::ReadColumn<int64_t>(batch, 5);
    for (uint32_t row = 0; row < batch.size(); ++row) {
      result.totals[static_cast<size_t>(ids[row])] = totals[row];
    }
  }
  EXPECT_TRUE(pipeline.status(*state).ok())
      << pipeline.status(*state).message();
  return result;
}

TEST(TreeAccumulateTest, ChildFirstDoesNotRequireDfsPostOrder) {
  // The two leaves arrive before either of their parents, interleaving the
  // subtrees. Run without an ordering step to preserve this exact order,
  // including when the pending totals must survive a batch boundary.
  for (uint32_t chunk : {1u, 2u, 5u}) {
    EXPECT_THAT(Accumulate({-1, 0, 0, 1, 2}, {1, 2, 3, 4, 5}, chunk,
                           /*up=*/true, {3, 4, 1, 2, 0},
                           /*already_ordered=*/true)
                    .totals,
                ElementsAre(15, 6, 8, 4, 5))
        << "chunk " << chunk;
  }
}

// One batch of node, parent and value columns; `valid` null if no value is.
RowBatch Batch(const std::vector<uint32_t>& nodes,
               const std::vector<uint32_t>& parents,
               const std::vector<int64_t>& values,
               const BitVector* valid = nullptr) {
  RowBatch in;
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, nodes.data()));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data(), valid));
  in.SetRowCount(static_cast<uint32_t>(nodes.size()));
  return in;
}

TEST(TreeAccumulateTest, IntegerOverflowIsReported) {
  // Child first for up, parent first for down.
  std::vector<uint32_t> up_nodes = {1, 0};
  std::vector<uint32_t> up_parents = {0, kNoNode};
  std::vector<uint32_t> down_nodes = {0, 1};
  std::vector<uint32_t> down_parents = {kNoNode, 0};
  std::vector<int64_t> values = {std::numeric_limits<int64_t>::max(), 1};
  RowBatch out;

  TreeAccumulateDown down({0, 1, {Sum(2)}});
  std::unique_ptr<OperatorState> down_state =
      down.MakeState(test::TestContext());
  RowBatch in = Batch(down_nodes, down_parents, values);
  EXPECT_FALSE(test::ProcessCopy(down, in, out, *down_state));
  EXPECT_THAT(down.status(*down_state).message(),
              testing::HasSubstr("overflow"));

  TreeAccumulateUp up({0, 1, {Sum(2)}});
  std::unique_ptr<OperatorState> up_state = up.MakeState(test::TestContext());
  in = Batch(up_nodes, up_parents, values);
  EXPECT_FALSE(test::ProcessCopy(up, in, out, *up_state));
  EXPECT_THAT(up.status(*up_state).message(), testing::HasSubstr("overflow"));
  values[0] = 1;
  up_state->Reset();
  EXPECT_TRUE(test::ProcessCopy(up, in, out, *up_state));
  EXPECT_TRUE(up.status(*up_state).ok());
}

TEST(TreeAccumulateTest, NullsAreSkipped) {
  // A root with no value and two children, either way round: an aggregate
  // of only nulls is null.
  AggregateCall min{AggregateCall::Function::kMin, 2};
  AggregateCall max{AggregateCall::Function::kMax, 2};
  std::vector<uint32_t> up_nodes = {1, 2, 0};
  std::vector<uint32_t> up_parents = {0, 0, kNoNode};
  std::vector<int64_t> up_values = {-3, 4, 0};
  BitVector up_valid = BitVector::CreateWithSize(3);
  up_valid.set(0);
  up_valid.set(1);
  TreeAccumulateUp up({0, 1, {min, max}});
  std::unique_ptr<OperatorState> up_state = up.MakeState(test::TestContext());
  RowBatch out;
  RowBatch in = Batch(up_nodes, up_parents, up_values, &up_valid);
  ASSERT_TRUE(test::ProcessCopy(up, in, out, *up_state));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 3),
              ElementsAre(-3, 4, -3));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 4), ElementsAre(-3, 4, 4));

  std::vector<uint32_t> down_nodes = {0, 1, 2};
  std::vector<uint32_t> down_parents = {kNoNode, 0, 0};
  std::vector<int64_t> down_values = {0, -3, 4};
  BitVector down_valid = BitVector::CreateWithSize(3);
  down_valid.set(1);
  down_valid.set(2);
  TreeAccumulateDown down({0, 1, {min, max, Sum(2)}});
  std::unique_ptr<OperatorState> down_state =
      down.MakeState(test::TestContext());
  in = Batch(down_nodes, down_parents, down_values, &down_valid);
  ASSERT_TRUE(test::ProcessCopy(down, in, out, *down_state));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 3),
              ElementsAre(std::nullopt, -3, 4));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 4),
              ElementsAre(std::nullopt, -3, 4));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 5),
              ElementsAre(std::nullopt, -3, 4));
}

TEST(TreeAccumulateTest, NullsFirstMetInALaterBatchKeepEarlierValues) {
  // Children with values, then a root with none.
  std::vector<uint32_t> children = {1, 2};
  std::vector<uint32_t> to_root = {0, 0};
  std::vector<int64_t> child_values = {3, 4};
  std::vector<uint32_t> root = {0};
  std::vector<uint32_t> no_parent = {kNoNode};
  std::vector<int64_t> root_value = {99};
  BitVector none = BitVector::CreateWithSize(1);
  TreeAccumulateUp op({0, 1, {Sum(2)}});
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  RowBatch out;
  RowBatch first = Batch(children, to_root, child_values);
  ASSERT_TRUE(test::ProcessCopy(op, first, out, *state));
  RowBatch second = Batch(root, no_parent, root_value, &none);
  ASSERT_TRUE(test::ProcessCopy(op, second, out, *state));
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(out, 3), ElementsAre(7));
}

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

// Random forests, against the folds written out directly: in order and
// shuffled, so the ordering operators buffer them; in batches of one row and
// up; and too big for one of the store's chunks, so each batch handed back
// draws its rows from more than one.
TEST(TreeAccumulateTest, MatchesTheDefinitions) {
  std::mt19937 rng(11);
  struct Case {
    uint32_t rows;
    uint32_t chunk_rows;
  };
  for (Case c : {Case{1, 1}, Case{300, 1}, Case{300, 7}, Case{300, 64},
                 Case{kMaxBatchRows * 2 + 137, 512}}) {
    std::vector<int64_t> parent = RandomParents(rng, c.rows);
    std::vector<int64_t> value(c.rows);
    for (uint32_t i = 0; i < c.rows; ++i) {
      value[i] = std::uniform_int_distribution<int64_t>(-50, 50)(rng);
    }
    std::vector<uint32_t> shuffled(c.rows);
    for (uint32_t i = 0; i < c.rows; ++i) {
      shuffled[i] = i;
    }
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    for (const std::vector<uint32_t>& order :
         {std::vector<uint32_t>{}, shuffled}) {
      EXPECT_EQ(
          Accumulate(parent, value, c.chunk_rows, /*up=*/true, order).totals,
          ReferenceUp(parent, value))
          << c.rows << " rows in " << c.chunk_rows;
      EXPECT_EQ(
          Accumulate(parent, value, c.chunk_rows, /*up=*/false, order).totals,
          ReferenceDown(parent, value))
          << c.rows << " rows in " << c.chunk_rows;
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
