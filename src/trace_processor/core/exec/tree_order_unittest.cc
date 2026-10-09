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
#include <variant>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

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
    test::AddCopy(*s.context, s.ids, nullptr, &out);
    test::AddCopy(*s.context, s.parents, &s.validity, &out);
    test::AddCopy(*s.context, s.payloads, nullptr, &out);
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

  std::unique_ptr<OperatorState> MakeState(Context&) const override {
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
    out.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Uint32{}}, nodes_.data()));
    out.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Uint32{}}, parents_.data()));
    out.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, payloads_.data()));
    out.SetRowCount(static_cast<uint32_t>(nodes_.size()));
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
      : source_(source), state_(source.MakeState(test::TestContext())) {}

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

std::vector<Pipeline::Step> Number() {
  std::vector<Pipeline::Step> ops;
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

enum class Direction { kChildFirst, kParentFirst };
constexpr Direction kDirections[] = {Direction::kChildFirst,
                                     Direction::kParentFirst};

Pipeline::Step Order(Direction direction, uint32_t node, uint32_t parent) {
  if (direction == Direction::kChildFirst) {
    return std::make_unique<TreeChildFirst>(node, parent);
  }
  return std::make_unique<TreeParentFirst>(node, parent);
}

// Numbers the rows, then orders them.
std::vector<Pipeline::Step> NumberAndOrder(Direction direction) {
  std::vector<Pipeline::Step> ops = Number();
  ops.push_back(Order(direction, 3, 4));
  return ops;
}

// Every row comes out after its parent if parent first, or before it if
// child first; and every row comes out once.
void ExpectOrdered(const Output& out, Direction direction, size_t rows) {
  ASSERT_EQ(out.node.size(), rows);
  std::vector<bool> seen(rows);
  for (uint32_t i = 0; i < out.node.size(); ++i) {
    if (out.parent[i] >= 0) {
      EXPECT_EQ(seen[static_cast<size_t>(out.parent[i])],
                direction == Direction::kParentFirst)
          << "row " << i;
    }
    EXPECT_FALSE(seen[static_cast<size_t>(out.node[i])]);
    seen[static_cast<size_t>(out.node[i])] = true;
  }
}

// Rows already child first pass through, rows parent first are turned round,
// and rows in neither order are sorted.
TEST(TreeChildFirstTest, PassesThroughReversesOrSortsTheRows) {
  struct Case {
    std::vector<Row> rows;
    // Empty when any child-first order will do.
    std::vector<int64_t> payload;
  };
  std::vector<Case> cases = {
      {ChildFirstRows(), {103, 102, 101, 100}},
      // Child first, but with subtrees interleaved.
      {{{3, 1, 3}, {4, 2, 4}, {1, 0, 1}, {2, 0, 2}, {0, std::nullopt, 0}},
       {3, 4, 1, 2, 0}},
      {ParentFirstRows(), {103, 102, 101, 100}},
      // 1 before its parent 0, then 3 after its parent 2.
      {{{1, 0, 101}, {0, std::nullopt, 100}, {2, 0, 102}, {3, 2, 103}}, {}},
  };
  for (const Case& c : cases) {
    RowSource source(c.rows, 2);
    Pipeline order(source, NumberAndOrder(Direction::kChildFirst), {});
    Output out = Drain(order);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    ExpectOrdered(out, Direction::kChildFirst, c.rows.size());
    if (!c.payload.empty()) {
      EXPECT_EQ(out.payload, c.payload);
    }
  }
}

// More rows let go at once than fit in a batch come out over several.
TEST(TreeParentFirstTest, RowsLetGoSpanBatches) {
  std::vector<Row> rows;
  for (int64_t id = 1; id <= kMaxBatchRows * 2 + 5; ++id) {
    rows.push_back({id, 0, id});
  }
  rows.push_back({0, std::nullopt, 0});
  RowSource source(rows, 1000);
  Pipeline order(source, NumberAndOrder(Direction::kParentFirst), {});

  Output out = Drain(order);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_EQ(out.payload.front(), 0);
  ExpectOrdered(out, Direction::kParentFirst, rows.size());
}

TEST(TreeOrderTest, AShuffledTreeComesOutInOrder) {
  std::mt19937 rng(29);
  std::vector<Row> rows;
  rows.push_back({0, std::nullopt, 0});
  for (int64_t id = 1; id < 3000; ++id) {
    int64_t parent = std::uniform_int_distribution<int64_t>(0, id - 1)(rng);
    rows.push_back({id, parent, id});
  }
  std::shuffle(rows.begin(), rows.end(), rng);

  for (Direction direction : kDirections) {
    RowSource source(rows, 256);
    Pipeline order(source, NumberAndOrder(direction), {});
    Output out = Drain(order);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    ExpectOrdered(out, direction, rows.size());
  }
}

TEST(TreeOrderTest, ABadTreeIsReported) {
  struct Case {
    std::vector<Row> rows;
    const char* child_first;
    const char* parent_first;
  };
  std::vector<Case> cases = {
      {{{0, std::nullopt, 100}, {1, 42, 101}},
       "not itself a row",
       "not itself a row"},
      // Parent first, neither row is ever let go.
      {{{0, 1, 100}, {1, 0, 101}}, "cycle", "not itself a row"},
      {{{0, 0, 100}}, "own parent", "own parent"},
  };
  for (Direction direction : kDirections) {
    for (const Case& c : cases) {
      RowSource source(c.rows, 2);
      Pipeline order(source, NumberAndOrder(direction), {});
      Output out = Drain(order);
      EXPECT_FALSE(out.status.ok());
      EXPECT_THAT(out.status.message(),
                  testing::HasSubstr(direction == Direction::kChildFirst
                                         ? c.child_first
                                         : c.parent_first));
    }
    // Numbered elsewhere, two rows can claim the same node.
    NumberedSource source({0, 1, 0}, {1, kNoNode, 1}, {100, 101, 102});
    std::vector<Pipeline::Step> ops;
    ops.push_back(Order(direction, 0, 1));
    Pipeline order(source, std::move(ops), {});
    Execution run(order);
    while (run.Next()) {
    }
    EXPECT_FALSE(run.status().ok());
    EXPECT_THAT(run.status().message(), testing::HasSubstr("same node"));
  }
}

inline constexpr auto kIdTree = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id", "payload"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     NonNull{},
                                     IdSorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{}, DenseNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}));

// Passing rows keep an implicit id while released rows carry a gathered one,
// so a consumer retaining both sees the id column change representation.
TEST(TreeOrderTest, DownThenUpOverAnImplicitIdColumn) {
  StringPool pool;
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kIdTree, &pool);
  df.InsertUnchecked(kIdTree, std::monostate{}, std::optional<uint32_t>(1),
                     int64_t{100});
  df.InsertUnchecked(kIdTree, std::monostate{}, std::optional<uint32_t>(),
                     int64_t{101});
  df.InsertUnchecked(kIdTree, std::monostate{}, std::optional<uint32_t>(1),
                     int64_t{102});
  df.Finalize();

  DataframeScan scan(
      {df.shared_column(0), df.shared_column(1), df.shared_column(2)},
      df.row_count());
  auto ops = Number();
  ops.push_back(std::make_unique<TreeParentFirst>(3, 4));
  ops.push_back(std::make_unique<TreeChildFirst>(3, 4));
  Pipeline pipeline(scan, std::move(ops), {});

  Execution run(pipeline);
  std::vector<uint32_t> ids;
  while (RowBatch* batch = run.Next()) {
    std::vector<uint32_t> batch_ids = test::ReadColumn<uint32_t>(*batch, 0);
    ids.insert(ids.end(), batch_ids.begin(), batch_ids.end());
  }
  ASSERT_TRUE(run.status().ok()) << run.status().message();
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids.back(), 1u);
  EXPECT_THAT(ids, testing::UnorderedElementsAre(0u, 1u, 2u));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
