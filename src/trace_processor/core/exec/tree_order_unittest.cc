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

#include "src/trace_processor/core/exec/tree_order.h"

#include <algorithm>
#include <cstdint>
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
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A row as the tests write one down: an id, a parent id or none, and a
// payload to prove the row itself came back out with its number.
struct Row {
  int64_t id;
  std::optional<int64_t> parent;
  int64_t payload;
};

// Hands the rows over in chunks, refilling one batch each time.
class RowSource final : public Source {
 public:
  RowSource(std::vector<Row> rows, uint32_t chunk_rows)
      : rows_(std::move(rows)), chunk_rows_(chunk_rows) {}

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
    s.ids.resize(count);
    s.parents.resize(count);
    s.payloads.resize(count);
    s.validity = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const Row& row = rows_[s.offset + i];
      s.ids[i] = row.id;
      s.payloads[i] = row.payload;
      s.parents[i] = row.parent.value_or(0);
      if (row.parent) {
        s.validity.set(i);
      }
    }
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.ids.data()));
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.parents.data(),
                                        &s.validity));
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, s.payloads.data()));
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
    std::vector<int64_t> payloads;
    BitVector validity;
  };

  std::vector<Row> rows_;
  uint32_t chunk_rows_;
};

RowSource::State::~State() = default;

// Drives a plan the way an executor does: it makes the state and owns the
// batch, so the plan itself stays const.
class Execution {
 public:
  explicit Execution(const Source& source)
      : source_(source), state_(source.MakeState()) {}

  RowBatch* Next() {
    return source_.GetData(batch_, *state_) ? &batch_ : nullptr;
  }
  base::Status status() const { return source_.status(*state_); }

 private:
  const Source& source_;
  std::unique_ptr<OperatorState> state_;
  RowBatch batch_;
};

// What came out: the payload, the node number and the parent's, in the order
// the rows were handed over.
struct Output {
  std::vector<int64_t> payload;
  std::vector<int64_t> node;
  std::vector<int64_t> parent;
  // Carried out with the rows, because how a run ended belongs to that run
  // and not to the plan.
  base::Status status = base::OkStatus();
};

int64_t Read(const RowBatch& batch, uint32_t column, uint32_t row) {
  const ColumnView& view = batch.column(column);
  return static_cast<const int64_t*>(
      view.data())[view.selection().GetIndex(row)];
}

// Node numbers, put on the end by TREE NUMBER NODES.
int64_t ReadNode(const RowBatch& batch, uint32_t column, uint32_t row) {
  const ColumnView& view = batch.column(column);
  auto value =
      static_cast<const uint32_t*>(view.data())[view.selection().GetIndex(row)];
  return value == kNoNode ? -1 : static_cast<int64_t>(value);
}

Output Drain(const TreeOrder&, Execution* run) {
  Output out;
  while (RowBatch* batch = run->Next()) {
    for (uint32_t row = 0; row < batch->size(); ++row) {
      out.payload.push_back(Read(*batch, 2, row));
      out.node.push_back(ReadNode(*batch, 3, row));
      out.parent.push_back(ReadNode(*batch, 4, row));
    }
  }
  out.status = run->status();
  return out;
}

// Most tests only want the rows back, so they get one execution and drain it.
Output Drain(const TreeOrder& order) {
  Execution run(order);
  return Drain(order, &run);
}

// A root, its two children, and a grandchild, written parent first.
std::vector<Row> ParentFirstRows() {
  return {{0, std::nullopt, 100}, {1, 0, 101}, {2, 0, 102}, {3, 1, 103}};
}

// The same tree, written child first.
std::vector<Row> ChildFirstRows() {
  return {{3, 1, 103}, {2, 0, 102}, {1, 0, 101}, {0, std::nullopt, 100}};
}

TEST(TreeOrderTest, RowsAlreadyInOrderArePassedThrough) {
  RowSource source(ParentFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_TRUE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
  EXPECT_THAT(out.node, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(out.parent, ElementsAre(-1, 0, 0, 1));
}

TEST(TreeOrderTest, RowsInTheOtherOrderAreTurnedRound) {
  RowSource source(ChildFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4, TreeRowOrder::kChildFirst);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_FALSE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
  // Every parent is now handed over before its children.
  for (uint32_t i = 0; i < out.node.size(); ++i) {
    if (out.parent[i] < 0) {
      continue;
    }
    auto seen =
        std::find(out.node.begin(), out.node.begin() + i, out.parent[i]);
    EXPECT_NE(seen, out.node.begin() + i) << "row " << i;
  }
}

TEST(TreeOrderTest, ChildFirstTurnsRoundRowsWhichArriveParentFirst) {
  RowSource source(ParentFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeChildFirst order(numbered, 3, 4, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_FALSE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
}

TEST(TreeOrderTest, ChildFirstPassesThroughRowsWhichAlreadyAre) {
  RowSource source(ChildFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeChildFirst order(numbered, 3, 4, TreeRowOrder::kChildFirst);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_TRUE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
}

// Without being told, the rows have to be held: by the time a row proves a
// guess wrong the rows before it have already gone.
TEST(TreeOrderTest, WithoutBeingToldTheRowsAreHeld) {
  RowSource source(ParentFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_FALSE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
}

TEST(TreeOrderTest, BeingToldWrongIsReported) {
  RowSource source(ChildFirstRows(), 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("they do not"));
}

// Ids which are a scattering -- a filtered set, say -- are numbered densely,
// so what is indexed by node number is the size of the input and not of
// whatever the input was filtered from.
TEST(TreeOrderTest, AScatteringOfIdsIsNumberedDensely) {
  std::vector<Row> rows = {{500, std::nullopt, 100},
                           {900, 500, 101},
                           {700, 500, 102},
                           {123, 900, 103}};
  RowSource source(rows, 3);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.node, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(out.parent, ElementsAre(-1, 0, 0, 1));
}

TEST(TreeOrderTest, AParentWhichIsNotARowIsReported) {
  std::vector<Row> rows = {{0, std::nullopt, 100}, {1, 42, 101}};
  RowSource source(rows, 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("not itself a row"));
}

// Every parent appears before all of its children.
void ExpectParentFirst(const Output& out) {
  std::vector<int64_t> seen;
  for (uint32_t i = 0; i < out.node.size(); ++i) {
    if (out.parent[i] >= 0) {
      EXPECT_NE(std::find(seen.begin(), seen.end(), out.parent[i]), seen.end())
          << "row " << i << " came before its parent";
    }
    seen.push_back(out.node[i]);
  }
}

// Rows in neither order are neither passed through nor turned round: they
// have to be put in an order which did not exist in the input.
TEST(TreeOrderTest, RowsInNeitherOrderAreSorted) {
  // 1 before its parent 0, then 3 after its parent 2.
  std::vector<Row> rows = {
      {1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 2, 103}};
  RowSource source(rows, 4);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ExpectParentFirst(out);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  EXPECT_THAT(payload, ElementsAre(100, 101, 102, 103));
}

TEST(TreeOrderTest, ASortedStreamCanBeAskedForTheOtherWayRound) {
  std::vector<Row> rows = {
      {1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 2, 103}};
  RowSource source(rows, 4);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  std::reverse(out.node.begin(), out.node.end());
  std::reverse(out.parent.begin(), out.parent.end());
  ExpectParentFirst(out);
}

TEST(TreeOrderTest, ACycleIsReported) {
  std::vector<Row> rows = {{0, 1, 100}, {1, 0, 101}};
  RowSource source(rows, 2);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("cycle"));
}

// A tree big enough to span batches, with its rows shuffled into no order at
// all.
TEST(TreeOrderTest, SortsATreeWhoseRowsAreShuffled) {
  std::mt19937 rng(11);
  std::vector<Row> rows;
  rows.push_back({0, std::nullopt, 1000});
  for (int64_t id = 1; id < 5000; ++id) {
    int64_t parent = std::uniform_int_distribution<int64_t>(0, id - 1)(rng);
    rows.push_back({id, parent, 1000 + id});
  }
  std::shuffle(rows.begin(), rows.end(), rng);

  RowSource source(rows, 512);
  std::vector<std::unique_ptr<Operator>> numbering;
  numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  Pipeline numbered(source, std::move(numbering));
  TreeParentFirst order(numbered, 3, 4);
  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ASSERT_EQ(out.node.size(), 5000u);
  ExpectParentFirst(out);

  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  for (uint32_t i = 0; i < payload.size(); ++i) {
    ASSERT_EQ(payload[i], 1000 + int64_t{i});
  }
}

TEST(TreeOrderTest, TheChunkSizeDoesNotChangeTheAnswer) {
  for (uint32_t chunk : {1u, 2u, 3u, 4u, 64u}) {
    RowSource source(ChildFirstRows(), chunk);
    std::vector<std::unique_ptr<Operator>> numbering;
    numbering.push_back(std::make_unique<TreeNumberNodes>(0, 1));
    Pipeline numbered(source, std::move(numbering));
    TreeParentFirst order(numbered, 3, 4, TreeRowOrder::kChildFirst);
    Output out = Drain(order);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103))
        << "chunk size " << chunk;
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
