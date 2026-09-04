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
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A row as the tests write one: an id, a parent id or none, and a payload
// proving the row itself came back out alongside its number.
struct Row {
  int64_t id;
  std::optional<int64_t> parent;
  int64_t payload;
};

// Emits the rows in batches, refilling a single batch each time.
class RowSource final : public Source {
 public:
  RowSource(std::vector<Row> rows, uint32_t chunk_rows)
      : rows_(std::move(rows)), chunk_rows_(chunk_rows) {}

  void SetRows(std::vector<Row> rows) { rows_ = std::move(rows); }

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

class NumberedSource final : public Source {
 public:
  NumberedSource(std::vector<uint32_t> nodes,
                 std::vector<uint32_t> parents,
                 std::vector<int64_t> payloads)
      : nodes_(std::move(nodes)),
        parents_(std::move(parents)),
        payloads_(std::move(payloads)) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = false;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    if (s.emitted) {
      return false;
    }
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, nodes_.data()));
    out.AddColumn(
        ColumnView::Reference(StorageType{Uint32{}}, parents_.data()));
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, payloads_.data()));
    out.SetCardinality(static_cast<uint32_t>(nodes_.size()));
    s.emitted = true;
    return true;
  }

 private:
  struct State : OperatorState {
    bool emitted = false;
  };

  std::vector<uint32_t> nodes_;
  std::vector<uint32_t> parents_;
  std::vector<int64_t> payloads_;
};

// Drives a plan the way an executor does: it creates the state and owns the
// batch, leaving the plan const.
class Execution {
 public:
  explicit Execution(const Source& source)
      : source_(source), state_(source.MakeState()) {}

  RowBatch* Next() {
    return source_.GetData(batch_, *state_) ? &batch_ : nullptr;
  }
  base::Status status() const { return source_.status(*state_); }
  void Rewind() { source_.Rewind(*state_); }

 private:
  const Source& source_;
  std::unique_ptr<OperatorState> state_;
  RowBatch batch_;
};

// The output: each row's payload, node number and parent node number, in the
// order the rows came out.
struct Output {
  std::vector<int64_t> payload;
  std::vector<int64_t> node;
  std::vector<int64_t> parent;
  // Returned with the rows, because how a run ended belongs to the run rather
  // than to the plan.
  base::Status status = base::OkStatus();
};

Output Drain(Execution* run) {
  Output out;
  while (RowBatch* batch = run->Next()) {
    std::vector<int64_t> payload = test::ReadColumn<int64_t>(*batch, 2);
    std::vector<uint32_t> nodes = test::ReadColumn<uint32_t>(*batch, 3);
    std::vector<uint32_t> parents = test::ReadColumn<uint32_t>(*batch, 4);
    out.payload.insert(out.payload.end(), payload.begin(), payload.end());
    for (uint32_t node : nodes) {
      out.node.push_back(node == kNoNode ? -1 : static_cast<int64_t>(node));
    }
    for (uint32_t parent : parents) {
      out.parent.push_back(parent == kNoNode ? -1
                                             : static_cast<int64_t>(parent));
    }
  }
  out.status = run->status();
  return out;
}

// Most tests only want the rows back, so they get one execution and drain it.
Output Drain(const Source& source) {
  Execution run(source);
  return Drain(&run);
}

std::vector<std::unique_ptr<Operator>> Number() {
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  return ops;
}

// A root, its two children and a grandchild, written parent first.
std::vector<Row> ParentFirstRows() {
  return {{0, std::nullopt, 100}, {1, 0, 101}, {2, 0, 102}, {3, 1, 103}};
}

// The same tree, written child first.
std::vector<Row> ChildFirstRows() {
  return {{3, 1, 103}, {2, 0, 102}, {1, 0, 101}, {0, std::nullopt, 100}};
}

// Every child appears before its parent.
void ExpectChildFirst(const Output& out) {
  std::vector<int64_t> seen;
  for (uint32_t i = 0; i < out.node.size(); ++i) {
    if (out.parent[i] >= 0) {
      EXPECT_EQ(std::find(seen.begin(), seen.end(), out.parent[i]), seen.end())
          << "row " << i << " came after its parent";
    }
    seen.push_back(out.node[i]);
  }
}

TEST(TreeChildFirstTest, RowsAlreadyInOrderComeBackAsTheyArrived) {
  RowSource source(ChildFirstRows(), 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
  EXPECT_THAT(out.node, ElementsAre(0, 2, 1, 3));
  EXPECT_THAT(out.parent, ElementsAre(1, 3, 3, -1));
}

TEST(TreeChildFirstTest, RowsInTheOtherOrderAreTurnedRound) {
  RowSource source(ParentFirstRows(), 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
  ExpectChildFirst(out);
}

// Rows in neither order can be neither passed through nor reversed: they have
// to be sorted into an order the input did not have.
TEST(TreeChildFirstTest, RowsInNeitherOrderAreSorted) {
  // 1 before its parent 0, then 3 after its parent 2.
  std::vector<Row> rows = {
      {1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 2, 103}};
  RowSource source(rows, 4);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ExpectChildFirst(out);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  EXPECT_THAT(payload, ElementsAre(100, 101, 102, 103));
}

// Scattered ids, such as those of a filtered relation, are numbered densely,
// so anything indexed by node number is the size of the input rather than of
// the table it was filtered from.
TEST(TreeChildFirstTest, AScatteringOfIdsIsNumberedDensely) {
  std::vector<Row> rows = {{123, 900, 103},
                           {700, 500, 102},
                           {900, 500, 101},
                           {500, std::nullopt, 100}};
  RowSource source(rows, 3);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.node, ElementsAre(0, 2, 1, 3));
  EXPECT_THAT(out.parent, ElementsAre(1, 3, 3, -1));
}

TEST(TreeChildFirstTest, AParentWhichIsNotARowIsReported) {
  std::vector<Row> rows = {{0, std::nullopt, 100}, {1, 42, 101}};
  RowSource source(rows, 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("not itself a row"));
}

TEST(TreeChildFirstTest, ACycleIsReported) {
  std::vector<Row> rows = {{0, 1, 100}, {1, 0, 101}};
  RowSource source(rows, 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("cycle"));
}

TEST(TreeChildFirstTest, ASelfParentIsReported) {
  RowSource source({{0, 0, 100}}, 1);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);

  Output out = Drain(order);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("own parent"));
}

TEST(TreeChildFirstTest, DuplicateNumberedNodesAreReported) {
  NumberedSource source({0, 1, 0}, {1, kNoNode, 1}, {100, 101, 102});
  TreeChildFirst order(source, 0, 1);
  Execution run(order);
  while (run.Next()) {
  }
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("same node"));
}

TEST(TreeChildFirstTest, RewindReadsTheInputAgain) {
  RowSource source(ChildFirstRows(), 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);
  Execution run(order);
  Output first = Drain(&run);
  ASSERT_TRUE(first.status.ok()) << first.status.message();

  source.SetRows(
      {{3, 1, 203}, {2, 0, 202}, {1, 0, 201}, {0, std::nullopt, 200}});
  run.Rewind();
  Output second = Drain(&run);
  ASSERT_TRUE(second.status.ok()) << second.status.message();
  EXPECT_THAT(second.payload, ElementsAre(203, 202, 201, 200));
}

TEST(TreeChildFirstTest, RewindDiscardsAFailedFill) {
  RowSource source({{0, 1, 100}, {1, 0, 101}}, 2);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);
  Execution run(order);
  Output failed = Drain(&run);
  ASSERT_FALSE(failed.status.ok());

  source.SetRows(ChildFirstRows());
  run.Rewind();
  Output recovered = Drain(&run);
  ASSERT_TRUE(recovered.status.ok()) << recovered.status.message();
  EXPECT_THAT(recovered.payload, ElementsAre(103, 102, 101, 100));
}

// Child first is not merely "every child before its parent": it is a depth
// first post-order, so a node's descendants are the block of rows immediately
// before it. A fold up the tree can therefore carry a stack of the current
// path rather than an array indexed by node.
TEST(TreeChildFirstTest, IsADepthFirstPostOrder) {
  std::mt19937 rng(29);
  std::vector<Row> rows;
  rows.push_back({0, std::nullopt, 0});
  for (int64_t id = 1; id < 3000; ++id) {
    int64_t parent = std::uniform_int_distribution<int64_t>(0, id - 1)(rng);
    rows.push_back({id, parent, id});
  }
  std::shuffle(rows.begin(), rows.end(), rng);

  RowSource source(rows, 256);
  Pipeline numbered(source, Number());
  TreeChildFirst order(numbered, 3, 4);
  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ASSERT_EQ(out.node.size(), rows.size());

  // Fold the tree up carrying only the current path, which is correct exactly
  // when the order is a post-order.
  std::vector<std::pair<int64_t, int64_t>> path;
  std::vector<int64_t> totals(rows.size(), 0);
  for (uint32_t i = 0; i < out.node.size(); ++i) {
    int64_t below = 0;
    if (!path.empty() && path.back().first == out.node[i]) {
      below = path.back().second;
      path.pop_back();
    }
    int64_t total = 1 + below;
    totals[static_cast<size_t>(out.payload[i])] = total;
    if (out.parent[i] >= 0) {
      if (path.empty() || path.back().first != out.parent[i]) {
        path.push_back({out.parent[i], 0});
      }
      path.back().second += total;
    }
  }
  EXPECT_TRUE(path.empty()) << "the path did not unwind";

  // Every node's total is the size of its subtree.
  std::vector<int64_t> expected(rows.size(), 1);
  std::vector<int64_t> parent_of(rows.size(), -1);
  for (const Row& row : rows) {
    parent_of[static_cast<size_t>(row.id)] =
        row.parent ? *row.parent : int64_t{-1};
  }
  for (size_t id = 0; id < rows.size(); ++id) {
    for (int64_t p = parent_of[id]; p >= 0;
         p = parent_of[static_cast<size_t>(p)]) {
      ++expected[static_cast<size_t>(p)];
    }
  }
  EXPECT_EQ(totals, expected);
}

// Numbers the rows, then puts them parent first.
std::vector<std::unique_ptr<Operator>> NumberParentFirst() {
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<TreeNumberNodes>(0, 1));
  ops.push_back(std::make_unique<TreeParentFirst>(3, 4));
  return ops;
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

TEST(TreeParentFirstTest, RowsInOrderStreamThrough) {
  RowSource source(ParentFirstRows(), 2);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
  EXPECT_THAT(out.node, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(out.parent, ElementsAre(-1, 0, 0, 1));
}

// A row arriving before its parent waits for it; the rest of the batch goes
// on ahead.
TEST(TreeParentFirstTest, ARowBeforeItsParentWaitsForIt) {
  std::vector<Row> rows = {
      {1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 1, 103}};
  RowSource source(rows, 4);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.payload, ElementsAre(100, 102, 101, 103));
  ExpectParentFirst(out);
}

TEST(TreeParentFirstTest, AHeldRowIsLetGoWhenItsParentArrivesLater) {
  RowSource source(ChildFirstRows(), 2);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ExpectParentFirst(out);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  EXPECT_THAT(payload, ElementsAre(100, 101, 102, 103));
}

// More rows let go at once than fit in a batch come out over several.
TEST(TreeParentFirstTest, RowsLetGoSpanBatches) {
  std::vector<Row> rows;
  for (int64_t id = 1; id <= kMaxBatchRows * 2 + 5; ++id) {
    rows.push_back({id, 0, id});
  }
  rows.push_back({0, std::nullopt, 0});
  RowSource source(rows, 1000);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ASSERT_EQ(out.payload.size(), rows.size());
  EXPECT_EQ(out.payload.front(), 0);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  for (size_t i = 0; i < payload.size(); ++i) {
    ASSERT_EQ(payload[i], static_cast<int64_t>(i));
  }
}

TEST(TreeParentFirstTest, AParentWhichIsNotARowIsReported) {
  RowSource source({{0, std::nullopt, 100}, {1, 42, 101}}, 2);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  EXPECT_THAT(out.payload, ElementsAre(100));
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("not itself a row"));
}

TEST(TreeParentFirstTest, ACycleIsReported) {
  RowSource source({{0, 1, 100}, {1, 0, 101}}, 2);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  EXPECT_TRUE(out.payload.empty());
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("not itself a row"));
}

TEST(TreeParentFirstTest, ASelfParentIsReported) {
  RowSource source({{0, 0, 100}}, 1);
  Pipeline pipeline(source, NumberParentFirst());

  Output out = Drain(pipeline);
  EXPECT_FALSE(out.status.ok());
  EXPECT_THAT(out.status.message(), testing::HasSubstr("own parent"));
}

TEST(TreeParentFirstTest, DuplicateNumberedNodesAreReported) {
  NumberedSource source({0, 1, 0}, {1, kNoNode, 1}, {100, 101, 102});
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<TreeParentFirst>(0, 1));
  Pipeline pipeline(source, std::move(ops));
  Execution run(pipeline);
  while (run.Next()) {
  }
  EXPECT_FALSE(run.status().ok());
  EXPECT_THAT(run.status().message(), testing::HasSubstr("same node"));
}

TEST(TreeParentFirstTest, RewindReadsTheInputAgain) {
  RowSource source(ChildFirstRows(), 2);
  Pipeline pipeline(source, NumberParentFirst());
  Execution run(pipeline);
  Output first = Drain(&run);
  ASSERT_TRUE(first.status.ok()) << first.status.message();

  source.SetRows(
      {{3, 1, 203}, {2, 0, 202}, {1, 0, 201}, {0, std::nullopt, 200}});
  run.Rewind();
  Output second = Drain(&run);
  ASSERT_TRUE(second.status.ok()) << second.status.message();
  ExpectParentFirst(second);
  std::vector<int64_t> payload = second.payload;
  std::sort(payload.begin(), payload.end());
  EXPECT_THAT(payload, ElementsAre(200, 201, 202, 203));
}

TEST(TreeParentFirstTest, RewindDiscardsHeldRows) {
  RowSource source({{0, std::nullopt, 100}, {1, 42, 101}}, 2);
  Pipeline pipeline(source, NumberParentFirst());
  Execution run(pipeline);
  Output failed = Drain(&run);
  ASSERT_FALSE(failed.status.ok());

  source.SetRows(ParentFirstRows());
  run.Rewind();
  Output recovered = Drain(&run);
  ASSERT_TRUE(recovered.status.ok()) << recovered.status.message();
  EXPECT_THAT(recovered.payload, ElementsAre(100, 101, 102, 103));
}

TEST(TreeParentFirstTest, AShuffledTreeComesOutParentFirst) {
  std::mt19937 rng(31);
  std::vector<Row> rows;
  rows.push_back({0, std::nullopt, 0});
  for (int64_t id = 1; id < 3000; ++id) {
    int64_t parent = std::uniform_int_distribution<int64_t>(0, id - 1)(rng);
    rows.push_back({id, parent, id});
  }
  std::shuffle(rows.begin(), rows.end(), rng);

  RowSource source(rows, 256);
  Pipeline pipeline(source, NumberParentFirst());
  Output out = Drain(pipeline);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ASSERT_EQ(out.node.size(), rows.size());
  ExpectParentFirst(out);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  for (size_t i = 0; i < payload.size(); ++i) {
    ASSERT_EQ(payload[i], static_cast<int64_t>(i));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
