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
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/util/graph.h"

namespace perfetto::trace_processor::core::exec {

namespace {

constexpr uint32_t kNoParent = util::GraphTree::kNoParent;

const char* Name(GraphTreeSpec::Kind kind) {
  switch (kind) {
    case GraphTreeSpec::Kind::kBfs:
      return "GRAPH BFS TREE";
    case GraphTreeSpec::Kind::kDfs:
      return "GRAPH DFS TREE";
    case GraphTreeSpec::Kind::kDominator:
      return "GRAPH DOMINATOR TREE";
  }
  PERFETTO_FATAL("For GCC");
}

// Finds a node by its id. Ids which are close to dense, as table ids are, are
// looked up in an array, and any others in a hash map.
class NodeIndex {
 public:
  // Numbers each of `ids` by its place. Returns an id which appears twice, if
  // any.
  std::optional<int64_t> Build(const std::vector<int64_t>& ids) {
    if (ids.empty()) {
      return std::nullopt;
    }
    auto [min, max] = std::minmax_element(ids.begin(), ids.end());
    min_ = *min;
    uint64_t span = static_cast<uint64_t>(*max) - static_cast<uint64_t>(*min);
    dense_ = span < 2 * uint64_t{ids.size()} + 1024;
    if (dense_) {
      node_of_offset_.assign(span + 1, kNoNode);
    }
    for (uint32_t node = 0; node < ids.size(); ++node) {
      if (dense_) {
        uint32_t& slot = node_of_offset_[static_cast<uint64_t>(ids[node]) -
                                         static_cast<uint64_t>(min_)];
        if (slot != kNoNode) {
          return ids[node];
        }
        slot = node;
      } else if (!node_of_id_.Insert(ids[node], node).second) {
        return ids[node];
      }
    }
    return std::nullopt;
  }

  // The node with `id`, or kNoNode.
  PERFETTO_ALWAYS_INLINE uint32_t Find(int64_t id) const {
    if (dense_) {
      auto offset = static_cast<uint64_t>(id) - static_cast<uint64_t>(min_);
      return offset < node_of_offset_.size() ? node_of_offset_[offset]
                                             : kNoNode;
    }
    const uint32_t* node = node_of_id_.Find(id);
    return node ? *node : kNoNode;
  }

 private:
  bool dense_ = false;
  int64_t min_ = 0;
  std::vector<uint32_t> node_of_offset_;
  base::FlatHashMap<int64_t, uint32_t> node_of_id_;
};

}  // namespace

struct GraphTree::State : OperatorState {
  ~State() override;

  struct Tree : util::GraphTree {
    using util::GraphTree::operator=;
    void EndRun() { static_cast<util::GraphTree&>(*this) = {}; }
  };

  std::unique_ptr<OperatorState> nodes_state;
  std::unique_ptr<OperatorState> edges_state;
  std::unique_ptr<OperatorState> seeds_state;
  Context* context = nullptr;

  Held<base::Status> status{this};

  // The inputs, read in full: what each batch is read into, and the retained
  // columns of every node row, in the order they arrived.
  Held<RowBatch> batch{this};
  Held<RowBatch> retained{this};
  Held<RowStore> rows{this};
  // Nodes are numbered in the order they arrived, so a node's number is also
  // its row in `rows`.
  Held<std::vector<int64_t>> id_of_node{this};

  // The tree, computed once.
  RunValue<bool> computed{this, false};
  Held<Tree> tree{this};
  // By node, its row's place in the output.
  Held<std::vector<uint32_t>> position{this};

  // The rows served.
  RunValue<uint32_t> served{this};
  Held<RowBatch> gathered{this};
  CallScratch<std::vector<uint32_t>> gather_rows;
};

GraphTree::State::~State() = default;

GraphTree::GraphTree(GraphTreeSpec spec) : spec_(std::move(spec)) {}

GraphTree::~GraphTree() = default;

std::unique_ptr<OperatorState> GraphTree::MakeState(Context& context) const {
  auto state = std::make_unique<State>();
  state->context = &context;
  state->nodes_state = spec_.nodes->MakeState(context);
  state->edges_state = spec_.edges->MakeState(context);
  state->seeds_state = spec_.seeds->MakeState(context);
  return state;
}

void GraphTree::Rewind(OperatorState& state) const {
  State& s = state.Cast<State>();
  Restart(*spec_.nodes, *s.nodes_state);
  Restart(*spec_.edges, *s.edges_state);
  Restart(*spec_.seeds, *s.seeds_state);
}

base::Status GraphTree::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

base::Status GraphTree::Build(State& s) const {
  const char* name = Name(spec_.kind);

  while (spec_.nodes->GetData(s.batch, *s.nodes_state)) {
    FlatColumnReader<int64_t> ids(s.batch, spec_.node_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        return base::ErrStatus("%s: a node has no id", name);
      }
      s.id_of_node.push_back(id);
    }
    s.retained.Project(s.batch, spec_.retained_columns);
    RETURN_IF_ERROR(s.rows.Append(s.retained, *s.context));
  }
  RETURN_IF_ERROR(spec_.nodes->status(*s.nodes_state));

  auto node_count = static_cast<uint32_t>(s.id_of_node.size());
  NodeIndex index;
  if (std::optional<int64_t> twice = index.Build(s.id_of_node)) {
    return base::ErrStatus("%s: two nodes have the id %" PRId64, name, *twice);
  }

  std::vector<uint32_t> seeds;
  while (spec_.seeds->GetData(s.batch, *s.seeds_state)) {
    FlatColumnReader<int64_t> ids(s.batch, spec_.seed_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t id;
      if (!ids.Read(row, &id)) {
        continue;
      }
      if (uint32_t node = index.Find(id); node != kNoNode) {
        seeds.push_back(node);
      }
    }
  }
  RETURN_IF_ERROR(spec_.seeds->status(*s.seeds_state));

  // A dominator tree has one root, so it grows from an extra node with an
  // edge to every seed.
  bool dominator = spec_.kind == GraphTreeSpec::Kind::kDominator;
  uint32_t graph_nodes = node_count + (dominator ? 1 : 0);
  std::vector<uint32_t> sources;
  std::vector<uint32_t> targets;
  while (spec_.edges->GetData(s.batch, *s.edges_state)) {
    FlatColumnReader<int64_t> from(s.batch, spec_.source_column);
    FlatColumnReader<int64_t> to(s.batch, spec_.target_column);
    for (uint32_t row = 0; row < s.batch.size(); ++row) {
      int64_t source;
      int64_t target;
      if (!from.Read(row, &source) || !to.Read(row, &target)) {
        continue;
      }
      uint32_t source_node = index.Find(source);
      uint32_t target_node = index.Find(target);
      if (source_node != kNoNode && target_node != kNoNode) {
        sources.push_back(source_node);
        targets.push_back(target_node);
      }
    }
  }
  RETURN_IF_ERROR(spec_.edges->status(*s.edges_state));
  if (dominator) {
    for (uint32_t seed : seeds) {
      sources.push_back(node_count);
      targets.push_back(seed);
    }
  }

  util::Digraph graph = util::Digraph::FromEdges(graph_nodes, sources, targets);

  switch (spec_.kind) {
    case GraphTreeSpec::Kind::kBfs:
      s.tree = util::BreadthFirstSearch(graph, seeds);
      break;
    case GraphTreeSpec::Kind::kDfs:
      s.tree = util::DepthFirstSearch(graph, seeds);
      break;
    case GraphTreeSpec::Kind::kDominator:
      s.tree = util::DominatorTree(graph, node_count);
      s.tree.order.erase(s.tree.order.begin());
      s.tree.parent.pop_back();
      for (uint32_t node : s.tree.order) {
        if (s.tree.parent[node] == node_count) {
          s.tree.parent[node] = kNoParent;
        }
      }
      break;
  }

  s.position.assign(node_count, kNoNode);
  for (uint32_t i = 0; i < s.tree.order.size(); ++i) {
    s.position[s.tree.order[i]] = i;
  }
  return base::OkStatus();
}

bool GraphTree::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (!s.computed) {
    s.status = Build(s);
    if (!s.status.ok()) {
      return false;
    }
    s.computed = true;
  }

  auto total = static_cast<uint32_t>(s.tree.order.size());
  uint32_t count = std::min(total - s.served, kMaxBatchRows);
  out.Reset();
  if (count == 0) {
    return false;
  }

  ColumnBuffer parent_ids = s.context->TakeBuffer(sizeof(int64_t));
  ColumnBuffer numbers = s.context->TakeBuffer(sizeof(uint32_t));
  ColumnBuffer parent_numbers = s.context->TakeBuffer(sizeof(uint32_t));
  ColumnChunk& parent_id_chunk = parent_ids.chunk();
  auto* parent_id = parent_id_chunk.Values<int64_t>();
  parent_id_chunk.validity.resize(kMaxBatchRows);
  auto* number = numbers.chunk().Values<uint32_t>();
  auto* parent_number = parent_numbers.chunk().Values<uint32_t>();
  s.gather_rows.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t node = s.tree.order[s.served + i];
    uint32_t parent = s.tree.parent[node];
    s.gather_rows[i] = node;
    number[i] = s.served + i;
    bool root = parent == kNoParent;
    parent_id[i] = root ? 0 : s.id_of_node[parent];
    parent_id_chunk.validity.change(i, !root);
    parent_number[i] = root ? kNoNode : s.position[parent];
  }
  s.served += count;

  const uint32_t* rows = s.gather_rows.data();
  s.rows.Gather(&s.gathered, {rows, rows + count}, *s.context);
  for (uint32_t c = 0; c < s.gathered.column_count(); ++c) {
    out.AddColumn(s.gathered.column(c), s.gathered.buffer(c));
  }
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, parent_id,
                                      &parent_id_chunk.validity),
                std::move(parent_ids));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, number),
                std::move(numbers));
  out.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parent_number),
                std::move(parent_numbers));
  out.SetRowCount(count);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
