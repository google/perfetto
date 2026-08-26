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
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/tree_order.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

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

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
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
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.ids.data()));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.parents.data(),
                                        &s.validity));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.values.data()));
    out.Compose(RowSelection::Range(0), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
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
                  std::optional<TreeRowOrder> arriving = std::nullopt) {
  RowSource source(parent, value, chunk_rows, std::move(order));
  // Ids become node numbers before anything else sees them.
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));

  std::unique_ptr<TreeOrder> ordered;
  if (up) {
    ordered = std::make_unique<TreeChildFirst>(numbered, 3, 4, arriving);
  } else {
    ordered = std::make_unique<TreeParentFirst>(numbered, 3, 4, arriving);
  }
  AccumulateSpec spec{3, 4, 2};
  std::vector<std::unique_ptr<Operator>> ops;
  if (up) {
    ops.push_back(std::make_unique<TreeAccumulateUp>(spec));
  } else {
    ops.push_back(std::make_unique<TreeAccumulateDown>(spec));
  }
  Pipeline pipeline(*ordered, std::move(ops));

  std::unique_ptr<OperatorState> state = pipeline.MakeState();
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

// A root, its two children and a grandchild.
std::vector<int64_t> Parents() {
  return {-1, 0, 0, 1};
}
std::vector<int64_t> Values() {
  return {1, 2, 3, 4};
}

TEST(TreeAccumulateTest, UpIsEverythingBelowANode) {
  EXPECT_THAT(Accumulate(Parents(), Values(), 8, /*up=*/true).totals,
              ElementsAre(10, 6, 3, 4));
}

TEST(TreeAccumulateTest, DownIsEverythingAboveANode) {
  EXPECT_THAT(Accumulate(Parents(), Values(), 8, /*up=*/false).totals,
              ElementsAre(1, 3, 4, 7));
}

TEST(TreeAccumulateTest, UpReportsIntegerOverflow) {
  std::vector<uint32_t> nodes = {1, 0};
  std::vector<uint32_t> parents = {0, kNoNode};
  std::vector<int64_t> values = {std::numeric_limits<int64_t>::max(), 1};
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, nodes.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetCardinality(2);

  TreeAccumulateUp op({0, 1, 2});
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch out;
  EXPECT_EQ(op.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("overflow"));
  op.Rewind(*state);
  EXPECT_TRUE(op.status(*state).ok());
}

TEST(TreeAccumulateTest, DownReportsIntegerOverflow) {
  std::vector<uint32_t> nodes = {0, 1};
  std::vector<uint32_t> parents = {kNoNode, 0};
  std::vector<int64_t> values = {std::numeric_limits<int64_t>::max(), 1};
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, nodes.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetCardinality(2);

  TreeAccumulateDown op({0, 1, 2});
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch out;
  EXPECT_EQ(op.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("overflow"));
}

TEST(TreeAccumulateTest, NullValuesContributeZero) {
  std::vector<uint32_t> nodes = {0, 1};
  std::vector<uint32_t> parents = {kNoNode, 0};
  std::vector<int64_t> values = {123, 7};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(1);
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, nodes.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  in.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data(), &validity));
  in.SetCardinality(2);

  TreeAccumulateDown op({0, 1, 2});
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch out;
  ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 3), ElementsAre(0, 7));
}

TEST(TreeAccumulateTest, WrongColumnTypesAreReported) {
  std::vector<int64_t> nodes = {0};
  std::vector<uint32_t> parents = {kNoNode};
  std::vector<int64_t> values = {1};
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, nodes.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  in.SetCardinality(1);

  TreeAccumulateDown op({0, 1, 2});
  std::unique_ptr<OperatorState> state = op.MakeState();
  RowBatch out;
  EXPECT_EQ(op.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("Uint32"));
}

// Nothing is buffered when the rows already arrive the right way round: one
// batch goes out for every batch that comes in. Any buffering is the ordering
// operator's doing, not the fold's.
TEST(TreeAccumulateTest, NothingIsBuffered) {
  std::vector<int64_t> parent(100, -1);
  std::vector<int64_t> value(100, 1);
  for (uint32_t i = 1; i < 100; ++i) {
    parent[i] = i - 1;
  }
  std::vector<uint32_t> ascending(100);
  std::vector<uint32_t> descending(100);
  for (uint32_t i = 0; i < 100; ++i) {
    ascending[i] = i;
    descending[i] = 99 - i;
  }
  EXPECT_EQ(Accumulate(parent, value, 10, /*up=*/true, descending,
                       TreeRowOrder::kChildFirst)
                .batches,
            10u);
  EXPECT_EQ(Accumulate(parent, value, 10, /*up=*/false, ascending,
                       TreeRowOrder::kParentFirst)
                .batches,
            10u);
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

TEST(TreeAccumulateTest, MatchesTheDefinitions) {
  std::mt19937 rng(11);
  for (int trial = 0; trial < 20; ++trial) {
    uint32_t rows = std::uniform_int_distribution<uint32_t>(1, 200)(rng);
    std::vector<int64_t> parent = RandomParents(rng, rows);
    std::vector<int64_t> value(rows);
    for (uint32_t i = 0; i < rows; ++i) {
      value[i] = std::uniform_int_distribution<int64_t>(-50, 50)(rng);
    }
    EXPECT_EQ(Accumulate(parent, value, 16, /*up=*/true).totals,
              ReferenceUp(parent, value));
    EXPECT_EQ(Accumulate(parent, value, 16, /*up=*/false).totals,
              ReferenceDown(parent, value));
  }
}

TEST(TreeAccumulateTest, TheChunkSizeDoesNotChangeTheAnswer) {
  std::mt19937 rng(3);
  std::vector<int64_t> parent = RandomParents(rng, 300);
  std::vector<int64_t> value(300, 2);
  for (uint32_t chunk : {1u, 2u, 7u, 64u, 1024u}) {
    EXPECT_EQ(Accumulate(parent, value, chunk, /*up=*/true).totals,
              ReferenceUp(parent, value))
        << "chunk " << chunk;
    EXPECT_EQ(Accumulate(parent, value, chunk, /*up=*/false).totals,
              ReferenceDown(parent, value))
        << "chunk " << chunk;
  }
}

// The running totals carried between batches have to be discarded when the
// plan is run again.
TEST(TreeAccumulateTest, RunningAgainStartsOver) {
  RowSource source(Parents(), Values(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeChildFirst order(numbered, 3, 4);
  AccumulateSpec spec{3, 4, 2};
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<TreeAccumulateUp>(spec));
  Pipeline pipeline(order, std::move(ops));

  std::unique_ptr<OperatorState> state = pipeline.MakeState();
  RowBatch batch;
  auto drain = [&] {
    std::vector<int64_t> totals(4, 0);
    while (pipeline.GetData(batch, *state)) {
      std::vector<int64_t> ids = test::ReadColumn<int64_t>(batch, 0);
      std::vector<int64_t> values = test::ReadColumn<int64_t>(batch, 5);
      for (uint32_t row = 0; row < batch.size(); ++row) {
        totals[static_cast<size_t>(ids[row])] = values[row];
      }
    }
    return totals;
  };
  std::vector<int64_t> first = drain();
  pipeline.Rewind(*state);
  EXPECT_EQ(drain(), first);
}

// A tree spanning several of the store's chunks, shuffled so that the ordering
// operator has to buffer all of it and hand it back in an order which draws
// each batch's rows from more than one chunk.
TEST(TreeAccumulateTest, ATreeTooBigForOneChunkIsStillFoldedRight) {
  std::mt19937 rng(23);
  std::vector<int64_t> parent = RandomParents(rng, kMaxBatchRows * 2 + 137);
  std::vector<int64_t> value(parent.size());
  for (uint32_t i = 0; i < value.size(); ++i) {
    value[i] = std::uniform_int_distribution<int64_t>(-30, 30)(rng);
  }
  std::vector<uint32_t> order(parent.size());
  for (uint32_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), rng);

  EXPECT_EQ(Accumulate(parent, value, 512, /*up=*/true, order).totals,
            ReferenceUp(parent, value));
  EXPECT_EQ(Accumulate(parent, value, 512, /*up=*/false, order).totals,
            ReferenceDown(parent, value));
}

// Rows in no particular order have to be sorted before either fold can read
// them, which is the ordering operator's job, not this one's.
TEST(TreeAccumulateTest, RowsInNoOrderAreStillFoldedRight) {
  std::mt19937 rng(19);
  std::vector<int64_t> parent = RandomParents(rng, 250);
  std::vector<int64_t> value(parent.size());
  for (uint32_t i = 0; i < value.size(); ++i) {
    value[i] = std::uniform_int_distribution<int64_t>(-30, 30)(rng);
  }
  std::vector<uint32_t> order(parent.size());
  for (uint32_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), rng);

  EXPECT_EQ(Accumulate(parent, value, 32, /*up=*/true, order).totals,
            ReferenceUp(parent, value));
  EXPECT_EQ(Accumulate(parent, value, 32, /*up=*/false, order).totals,
            ReferenceDown(parent, value));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
