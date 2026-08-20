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
#include <optional>
#include <random>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_batch_pool.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
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

  void Reset() override { offset_ = 0; }

  RowBatch* Next() override {
    auto total = static_cast<uint32_t>(rows_.size());
    if (offset_ == total) {
      return nullptr;
    }
    uint32_t count = std::min(chunk_rows_, total - offset_);
    ids_.resize(count);
    parents_.resize(count);
    payloads_.resize(count);
    validity_ = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      const Row& row = rows_[offset_ + i];
      ids_[i] = row.id;
      payloads_[i] = row.payload;
      parents_[i] = row.parent.value_or(0);
      if (row.parent) {
        validity_.set(i);
      }
    }
    batch_ = RowBatch();
    batch_.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids_.data()));
    batch_.AddColumn(ColumnView::Reference(StorageType{Int64{}},
                                           parents_.data(), &validity_));
    batch_.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, payloads_.data()));
    batch_.Compose(RowSelection::Range(0), count);
    batch_.SetCardinality(count);
    offset_ += count;
    return &batch_;
  }

 private:
  std::vector<Row> rows_;
  uint32_t chunk_rows_;
  uint32_t offset_ = 0;
  std::vector<int64_t> ids_;
  std::vector<int64_t> parents_;
  std::vector<int64_t> payloads_;
  BitVector validity_;
  RowBatch batch_;
};

// What came out: the payload, the node number and the parent's, in the order
// the rows were handed over.
struct Output {
  std::vector<int64_t> payload;
  std::vector<int64_t> node;
  std::vector<int64_t> parent;
};

int64_t Read(const RowBatch& batch, uint32_t column, uint32_t row) {
  const ColumnView& view = batch.column(column);
  return static_cast<const int64_t*>(
      view.data())[view.selection().GetIndex(row)];
}

Output Drain(TreeOrder& order) {
  Output out;
  while (RowBatch* batch = order.Next()) {
    for (uint32_t row = 0; row < batch->size(); ++row) {
      out.payload.push_back(Read(*batch, 2, row));
      out.node.push_back(Read(*batch, order.node_column(), row));
      out.parent.push_back(Read(*batch, order.parent_ordinal_column(), row));
    }
  }
  return out;
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
  TreeParentFirst order(source, 0, 1, 3, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  EXPECT_TRUE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
  EXPECT_THAT(out.node, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(out.parent, ElementsAre(-1, 0, 0, 1));
}

TEST(TreeOrderTest, RowsInTheOtherOrderAreTurnedRound) {
  RowSource source(ChildFirstRows(), 2);
  TreeParentFirst order(source, 0, 1, 3, TreeRowOrder::kChildFirst);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
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
  TreeChildFirst order(source, 0, 1, 3, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  EXPECT_FALSE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
}

TEST(TreeOrderTest, ChildFirstPassesThroughRowsWhichAlreadyAre) {
  RowSource source(ChildFirstRows(), 2);
  TreeChildFirst order(source, 0, 1, 3, TreeRowOrder::kChildFirst);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  EXPECT_TRUE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(103, 102, 101, 100));
}

// Without being told, the rows have to be held: by the time a row proves a
// guess wrong the rows before it have already gone.
TEST(TreeOrderTest, WithoutBeingToldTheRowsAreHeld) {
  RowSource source(ParentFirstRows(), 2);
  TreeParentFirst order(source, 0, 1, 3);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  EXPECT_FALSE(order.streamed());
  EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103));
}

TEST(TreeOrderTest, BeingToldWrongIsReported) {
  RowSource source(ChildFirstRows(), 2);
  TreeParentFirst order(source, 0, 1, 3, TreeRowOrder::kParentFirst);

  Drain(order);
  EXPECT_FALSE(order.status().ok());
  EXPECT_THAT(order.status().message(), testing::HasSubstr("they do not"));
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
  TreeParentFirst order(source, 0, 1, 3, TreeRowOrder::kParentFirst);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  EXPECT_THAT(out.node, ElementsAre(0, 1, 2, 3));
  EXPECT_THAT(out.parent, ElementsAre(-1, 0, 0, 1));
}

TEST(TreeOrderTest, AParentWhichIsNotARowIsReported) {
  std::vector<Row> rows = {{0, std::nullopt, 100}, {1, 42, 101}};
  RowSource source(rows, 2);
  TreeParentFirst order(source, 0, 1, 3);

  Drain(order);
  EXPECT_FALSE(order.status().ok());
  EXPECT_THAT(order.status().message(), testing::HasSubstr("not itself a row"));
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
  TreeParentFirst order(source, 0, 1, 3);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  ExpectParentFirst(out);
  std::vector<int64_t> payload = out.payload;
  std::sort(payload.begin(), payload.end());
  EXPECT_THAT(payload, ElementsAre(100, 101, 102, 103));
}

TEST(TreeOrderTest, ASortedStreamCanBeAskedForTheOtherWayRound) {
  std::vector<Row> rows = {
      {1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 2, 103}};
  RowSource source(rows, 4);
  TreeChildFirst order(source, 0, 1, 3);

  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
  std::reverse(out.node.begin(), out.node.end());
  std::reverse(out.parent.begin(), out.parent.end());
  ExpectParentFirst(out);
}

TEST(TreeOrderTest, ACycleIsReported) {
  std::vector<Row> rows = {{0, 1, 100}, {1, 0, 101}};
  RowSource source(rows, 2);
  TreeParentFirst order(source, 0, 1, 3);

  Drain(order);
  EXPECT_FALSE(order.status().ok());
  EXPECT_THAT(order.status().message(), testing::HasSubstr("cycle"));
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
  TreeParentFirst order(source, 0, 1, 3);
  Output out = Drain(order);
  ASSERT_TRUE(order.status().ok()) << order.status().message();
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
    TreeParentFirst order(source, 0, 1, 3, TreeRowOrder::kChildFirst);
    Output out = Drain(order);
    ASSERT_TRUE(order.status().ok()) << order.status().message();
    EXPECT_THAT(out.payload, ElementsAre(100, 101, 102, 103))
        << "chunk size " << chunk;
  }
}

// The point of the operator: a fold up the tree reads what this hands it,
// whether the rows arrived in that order, the other one, or none.
TEST(TreeOrderTest, WhatComesOutCanBeFoldedUp) {
  std::mt19937 rng(3);
  std::vector<Row> rows;
  rows.push_back({0, std::nullopt, 1});
  std::vector<int64_t> parent_of = {-1};
  for (int64_t id = 1; id < 400; ++id) {
    int64_t parent = std::uniform_int_distribution<int64_t>(0, id - 1)(rng);
    parent_of.push_back(parent);
    rows.push_back({id, parent, id + 1});
  }

  // What the answer is, by the definition: everything whose parent chain
  // reaches a node.
  std::vector<int64_t> want(rows.size(), 0);
  for (uint32_t row = 0; row < rows.size(); ++row) {
    for (int64_t walk = static_cast<int64_t>(row); walk >= 0;
         walk = parent_of[static_cast<size_t>(walk)]) {
      want[static_cast<size_t>(walk)] += static_cast<int64_t>(row) + 1;
    }
  }

  for (int arrival = 0; arrival < 3; ++arrival) {
    std::vector<Row> input = rows;
    if (arrival == 1) {
      std::reverse(input.begin(), input.end());
    } else if (arrival == 2) {
      std::shuffle(input.begin(), input.end(), rng);
    }
    RowSource source(input, 64);
    TreeParentFirst order(source, 0, 1, 3);
    RowBatchPool pool;
    TreeAccumulateUp fold(order, &pool, order.parent_ordinal_column(), 2,
                          order.node_column());

    std::vector<int64_t> got(rows.size(), 0);
    while (RowBatch* batch = fold.Next()) {
      for (uint32_t row = 0; row < batch->size(); ++row) {
        auto id = Read(*batch, 0, row);
        got[static_cast<size_t>(id)] = Read(*batch, 5, row);
      }
    }
    ASSERT_TRUE(fold.status().ok()) << fold.status().message();
    EXPECT_EQ(got, want) << "arrival: " << arrival;
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
