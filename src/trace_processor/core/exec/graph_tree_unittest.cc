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

#include "src/trace_processor/core/exec/graph_tree.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using Kind = GraphTreeSpec::Kind;
using Column = std::vector<std::optional<int64_t>>;

// Emits nullable Int64 columns, a fixed number of rows at a time.
class ColumnsSource final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit ColumnsSource(std::vector<Column> columns, uint32_t chunk_rows = 3)
      : columns_(std::move(columns)), chunk_rows_(chunk_rows) {}

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
    auto total = static_cast<uint32_t>(columns_.front().size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, total - s.offset);
    out.Reset();
    for (const Column& column : columns_) {
      std::vector<int64_t> values(count);
      BitVector present = BitVector::CreateWithSize(count, true);
      for (uint32_t row = 0; row < count; ++row) {
        const std::optional<int64_t>& value = column[s.offset + row];
        values[row] = value.value_or(0);
        present.change(row, value.has_value());
      }
      test::AddCopy(*s.context, values, &present, &out);
    }
    out.SetRowCount(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    Context* context = nullptr;
    uint32_t offset = 0;
  };

  std::vector<Column> columns_;
  uint32_t chunk_rows_;
};

ColumnsSource::State::~State() = default;

// A row out: the node's id and payload, its parent's id, and the row numbers.
struct Row {
  int64_t id;
  int64_t payload;
  std::optional<int64_t> parent;
  uint32_t number;
  uint32_t parent_number;

  bool operator==(const Row& o) const {
    return id == o.id && payload == o.payload && parent == o.parent &&
           number == o.number && parent_number == o.parent_number;
  }
  friend std::ostream& operator<<(std::ostream& os, const Row& r) {
    os << "{" << r.id << ", " << r.payload << ", ";
    if (r.parent) {
      os << *r.parent;
    } else {
      os << "null";
    }
    return os << ", " << r.number << ", " << r.parent_number << "}";
  }
};

class GraphTreeTest : public ::testing::Test {
 protected:
  // Nodes are (id, payload) and edges are (source, target).
  void SetGraph(Column ids,
                Column payloads,
                Column sources,
                Column targets,
                Column seeds) {
    nodes_ = std::make_unique<ColumnsSource>(
        std::vector<Column>{std::move(ids), std::move(payloads)});
    edges_ = std::make_unique<ColumnsSource>(
        std::vector<Column>{std::move(sources), std::move(targets)});
    seeds_ =
        std::make_unique<ColumnsSource>(std::vector<Column>{std::move(seeds)});
  }

  std::unique_ptr<GraphTree> Make(Kind kind) const {
    GraphTreeSpec spec;
    spec.kind = kind;
    spec.nodes = nodes_.get();
    spec.node_column = 0;
    spec.retained_columns = {0, 1};
    spec.edges = edges_.get();
    spec.source_column = 0;
    spec.target_column = 1;
    spec.seeds = seeds_.get();
    spec.seed_column = 0;
    return std::make_unique<GraphTree>(std::move(spec));
  }

  static std::vector<Row> Drain(const GraphTree& tree,
                                OperatorState& state,
                                base::Status* status = nullptr) {
    std::vector<Row> rows;
    RowBatch batch;
    while (tree.GetData(batch, state)) {
      EXPECT_EQ(batch.column_count(), 5u);
      for (uint32_t row = 0; row < batch.size(); ++row) {
        Row r;
        r.id = batch.Value<int64_t>(0, row);
        r.payload = batch.Value<int64_t>(1, row);
        if (batch.IsValid(2, row)) {
          r.parent = batch.Value<int64_t>(2, row);
        }
        r.number = batch.Value<uint32_t>(3, row);
        r.parent_number = batch.Value<uint32_t>(4, row);
        rows.push_back(r);
      }
    }
    if (status) {
      *status = tree.status(state);
    } else {
      EXPECT_TRUE(tree.status(state).ok()) << tree.status(state).message();
    }
    return rows;
  }

  std::vector<Row> Run(Kind kind, base::Status* status = nullptr) const {
    auto tree = Make(kind);
    auto state = tree->MakeState(test::TestContext());
    return Drain(*tree, *state, status);
  }

  // 3 -> 7, 1 -> 5, 9 -> 1, 7 -> 1, 5 -> 7, 1 -> 3, in that order, with the
  // nodes not in order of id.
  void SetExample(Column seeds) {
    SetGraph({7, 1, 9, 5, 3}, {70, 10, 90, 50, 30}, {3, 1, 9, 7, 5, 1},
             {7, 5, 1, 1, 7, 3}, std::move(seeds));
  }

  std::unique_ptr<ColumnsSource> nodes_;
  std::unique_ptr<ColumnsSource> edges_;
  std::unique_ptr<ColumnsSource> seeds_;
};

// 1 -> 5 arrives before 1 -> 3, so 5 is searched first and is 7's parent.
TEST_F(GraphTreeTest, BfsFollowsEdgesInTheOrderTheyArrive) {
  SetExample({1});
  EXPECT_THAT(
      Run(Kind::kBfs),
      ElementsAre(Row{1, 10, std::nullopt, 0, kNoNode}, Row{5, 50, 1, 1, 0},
                  Row{3, 30, 1, 2, 0}, Row{7, 70, 5, 3, 1}));
}

TEST_F(GraphTreeTest, DominatorEverySeedIsARoot) {
  SetExample({9, 1});
  EXPECT_THAT(
      Run(Kind::kDominator),
      ElementsAre(Row{9, 90, std::nullopt, 0, kNoNode},
                  Row{1, 10, std::nullopt, 1, kNoNode}, Row{5, 50, 1, 2, 1},
                  Row{7, 70, 1, 3, 1}, Row{3, 30, 1, 4, 1}));
}

TEST_F(GraphTreeTest, IgnoresEdgesAndSeedsNamingNoNode) {
  SetGraph({1, 2}, {10, 20}, {1, std::nullopt, 1, 4}, {4, 2, 2, 2},
           {std::nullopt, 8, 1, 1});
  EXPECT_THAT(Run(Kind::kBfs), ElementsAre(Row{1, 10, std::nullopt, 0, kNoNode},
                                           Row{2, 20, 1, 1, 0}));
}

TEST_F(GraphTreeTest, TwoNodesWithOneIdFail) {
  SetGraph({1, 2, 1}, {10, 20, 30}, {1}, {2}, {1});
  base::Status status;
  EXPECT_TRUE(Run(Kind::kDominator, &status).empty());
  EXPECT_THAT(status.message(),
              HasSubstr("GRAPH DOMINATOR TREE: two nodes have the id 1"));
}

TEST_F(GraphTreeTest, ManyBatchesAndRewind) {
  constexpr int64_t kNodes = 5000;
  Column ids;
  Column payloads;
  Column sources;
  Column targets;
  for (int64_t i = kNodes; i-- > 0;) {
    ids.emplace_back(i);
    payloads.emplace_back(i * 10);
    if (i > 0) {
      sources.emplace_back(i - 1);
      targets.emplace_back(i);
    }
  }
  SetGraph(std::move(ids), std::move(payloads), std::move(sources),
           std::move(targets), {0});
  auto tree = Make(Kind::kDfs);
  auto state = tree->MakeState(test::TestContext());
  for (int run = 0; run < 2; ++run) {
    std::vector<Row> rows = Drain(*tree, *state);
    ASSERT_EQ(rows.size(), static_cast<size_t>(kNodes));
    for (uint32_t i = 1; i < kNodes; ++i) {
      ASSERT_EQ(rows[i], (Row{i, i * 10, i - 1, i, i - 1}));
    }
    tree->Rewind(*state);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
