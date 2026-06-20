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

#include "src/trace_processor/duckdb/dominator_tree_function.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// ===========================================================================
// Lengauer-Tarjan dominator tree.
//
// This Node/TreeNumber/Graph/Forest block is a verbatim copy of the algorithm
// in src/trace_processor/plugins/dominator_tree/dominator_tree.cc (which lives
// in an anonymous namespace and so cannot be #included). It is duplicated here
// to keep the experimental DuckDB lane fully isolated from the production
// SQLite plugin; if/when the lane graduates, the two should be deduplicated
// behind a shared header. The only intentional change is replacing the
// SQLite-table output sink (`ToTable`) with `ForEachTreeNode`.
// ===========================================================================

class Forest;

struct Node {
  uint32_t id;
  bool operator==(const Node& v) const { return id == v.id; }
};

struct TreeNumber {
  uint32_t i;
  bool operator<(const TreeNumber& o) const { return i < o.i; }
};

class Graph {
 public:
  void RunDfs(Node root_node);
  void ComputeSemiDominatorAndPartialDominator(Forest&);
  void ComputeDominators();

  void AddEdge(Node source, Node dest) {
    state_by_node_.resize(std::max<size_t>(
        state_by_node_.size(), std::max(source.id + 1, dest.id + 1)));
    GetStateForNode(source).successors.push_back(dest);
    GetStateForNode(dest).predecessors.push_back(source);
  }

  // Replaces the plugin's ToTable: invokes `fn(node_id, dominator_id_or_null)`
  // for every node in the dominator tree, in tree order.
  template <typename Fn>
  void ForEachTreeNode(Node root_node, Fn&& fn) {
    for (uint32_t i = 0; i < node_count_in_tree(); ++i) {
      Node v = GetNodeForTreeNumber(TreeNumber{i});
      NodeState& v_state = GetStateForNode(v);
      std::optional<int64_t> dom =
          v == root_node ? std::nullopt
                         : std::make_optional<int64_t>(v_state.dominator.id);
      fn(static_cast<int64_t>(v.id), dom);
    }
  }

  TreeNumber GetSemiDominator(Node v) const {
    return *GetStateForNode(v).semi_dominator;
  }

  bool IsInTree(Node v) const {
    return GetStateForNode(v).semi_dominator.has_value();
  }

  uint32_t node_count_in_tree() const {
    return static_cast<uint32_t>(node_by_tree_number_.size());
  }

  uint32_t node_id_range() const {
    return static_cast<uint32_t>(state_by_node_.size());
  }

 private:
  struct NodeState {
    std::vector<Node> successors;
    std::vector<Node> predecessors;
    std::optional<TreeNumber> tree_parent;
    std::vector<Node> self_as_semi_dominator;
    std::optional<TreeNumber> semi_dominator;
    Node dominator{0};
  };

  const NodeState& GetStateForNode(Node v) const {
    return state_by_node_[v.id];
  }
  NodeState& GetStateForNode(Node v) { return state_by_node_[v.id]; }
  Node& GetNodeForTreeNumber(TreeNumber d) { return node_by_tree_number_[d.i]; }

  std::vector<NodeState> state_by_node_;
  std::vector<Node> node_by_tree_number_;
};

class Forest {
 public:
  explicit Forest(uint32_t vertices_count) : state_by_node_(vertices_count) {
    for (uint32_t i = 0; i < vertices_count; ++i) {
      state_by_node_[i].min_semi_dominator_until_ancestor = Node{i};
    }
  }

  void Link(Node ancestor, Node descendant) {
    std::optional<Node>& a = state_by_node_[descendant.id].ancestor;
    PERFETTO_DCHECK(!a);
    a = ancestor;
  }

  std::optional<Node> GetMinSemiDominatorToAncestor(Node vertex,
                                                    const Graph& graph) {
    if (!graph.IsInTree(vertex)) {
      return std::nullopt;
    }
    NodeState& state = GetStateForNode(vertex);
    if (!state.ancestor) {
      return vertex;
    }
    Compress(vertex, graph);
    return state.min_semi_dominator_until_ancestor;
  }

 private:
  struct NodeState {
    std::optional<Node> ancestor;
    Node min_semi_dominator_until_ancestor;
  };

  void Compress(Node vertex, const Graph& graph) {
    struct CompressState {
      Node current;
      bool recurse_done;
    };
    std::vector<CompressState> states{CompressState{vertex, false}};
    while (!states.empty()) {
      CompressState& s = states.back();
      NodeState& state = GetStateForNode(s.current);
      PERFETTO_CHECK(state.ancestor);
      NodeState& ancestor_state = GetStateForNode(*state.ancestor);
      if (s.recurse_done) {
        states.pop_back();
        Node ancestor_min = ancestor_state.min_semi_dominator_until_ancestor;
        Node self_min = state.min_semi_dominator_until_ancestor;
        if (graph.GetSemiDominator(ancestor_min) <
            graph.GetSemiDominator(self_min)) {
          state.min_semi_dominator_until_ancestor = ancestor_min;
        }
        state.ancestor = ancestor_state.ancestor;
      } else {
        s.recurse_done = true;
        if (auto grand_ancestor = ancestor_state.ancestor; grand_ancestor) {
          states.push_back(CompressState{*state.ancestor, false});
        } else {
          states.pop_back();
        }
      }
    }
  }

  NodeState& GetStateForNode(Node v) { return state_by_node_[v.id]; }

  std::vector<NodeState> state_by_node_;
};

void Graph::RunDfs(Node root) {
  struct StackState {
    Node node;
    std::optional<TreeNumber> parent;
  };

  std::vector<StackState> stack{{root, std::nullopt}};
  while (!stack.empty()) {
    StackState stack_state = stack.back();
    stack.pop_back();

    NodeState& s = GetStateForNode(stack_state.node);
    if (s.semi_dominator) {
      continue;
    }

    TreeNumber tree_number{static_cast<uint32_t>(node_by_tree_number_.size())};
    s.tree_parent = stack_state.parent;
    s.semi_dominator = tree_number;
    node_by_tree_number_.push_back(stack_state.node);

    for (auto it = s.successors.rbegin(); it != s.successors.rend(); ++it) {
      stack.emplace_back(StackState{*it, tree_number});
    }
  }
}

void Graph::ComputeSemiDominatorAndPartialDominator(Forest& forest) {
  for (uint32_t i = node_count_in_tree() - 1; i > 0; --i) {
    Node w = GetNodeForTreeNumber(TreeNumber{i});
    NodeState& w_state = GetStateForNode(w);
    for (Node v : w_state.predecessors) {
      auto u = forest.GetMinSemiDominatorToAncestor(v, *this);
      if (!u) {
        continue;
      }
      w_state.semi_dominator =
          std::min(*w_state.semi_dominator, GetSemiDominator(*u));
    }
    NodeState& semi_dominator_state =
        GetStateForNode(GetNodeForTreeNumber(*w_state.semi_dominator));
    semi_dominator_state.self_as_semi_dominator.push_back(w);
    PERFETTO_CHECK(w_state.tree_parent);

    Node w_parent = GetNodeForTreeNumber(*w_state.tree_parent);
    forest.Link(w_parent, w);

    NodeState& w_parent_state = GetStateForNode(w_parent);
    for (Node v : w_parent_state.self_as_semi_dominator) {
      Node u = *forest.GetMinSemiDominatorToAncestor(v, *this);
      NodeState& v_state = GetStateForNode(v);
      v_state.dominator =
          GetSemiDominator(u) < v_state.semi_dominator ? u : w_parent;
    }
    w_parent_state.self_as_semi_dominator.clear();
  }
}

void Graph::ComputeDominators() {
  for (uint32_t i = 1; i < node_count_in_tree(); ++i) {
    Node w = GetNodeForTreeNumber(TreeNumber{i});
    NodeState& w_state = GetStateForNode(w);
    Node semi_dominator = GetNodeForTreeNumber(*w_state.semi_dominator);
    if (w_state.dominator == semi_dominator) {
      continue;
    }
    w_state.dominator = GetStateForNode(w_state.dominator).dominator;
  }
}

// ===========================================================================
// DuckDB aggregate glue.
// ===========================================================================

struct ResultRow {
  int64_t node_id;
  int64_t dominator_id;
  bool has_dominator;
};

// Collected edges (in scan order) + the captured root node id.
struct DomBuffer {
  std::vector<int64_t> srcs;
  std::vector<int64_t> dsts;
  std::optional<int64_t> root;
};

std::vector<ResultRow> RunDominator(const DomBuffer& buf) {
  std::vector<ResultRow> out;
  // Mirror the plugin: an empty graph or an out-of-range root yields no rows.
  if (buf.srcs.empty() || !buf.root || *buf.root < 0) {
    return out;
  }
  Graph graph;
  for (size_t i = 0; i < buf.srcs.size(); ++i) {
    graph.AddEdge(Node{static_cast<uint32_t>(buf.srcs[i])},
                  Node{static_cast<uint32_t>(buf.dsts[i])});
  }
  Node root{static_cast<uint32_t>(*buf.root)};
  if (root.id >= graph.node_id_range()) {
    return out;
  }
  graph.RunDfs(root);
  // The plugin requires at least the root plus one other reachable node; a
  // smaller tree would underflow the semi-dominator loop. Return empty rather
  // than computing on a degenerate tree.
  if (graph.node_count_in_tree() <= 1) {
    return out;
  }
  Forest forest(graph.node_id_range());
  graph.ComputeSemiDominatorAndPartialDominator(forest);
  graph.ComputeDominators();
  graph.ForEachTreeNode(root, [&](int64_t node_id,
                                  std::optional<int64_t> dom) {
    out.push_back(ResultRow{node_id, dom ? *dom : 0, dom.has_value()});
  });
  return out;
}

using AggState = DomBuffer*;

idx_t StateSize(duckdb_function_info) {
  return sizeof(AggState);
}

void Init(duckdb_function_info, duckdb_aggregate_state state) {
  *reinterpret_cast<AggState*>(state) = nullptr;
}

void Update(duckdb_function_info,
            duckdb_data_chunk input,
            duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector src_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector dst_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector root_vec = duckdb_data_chunk_get_vector(input, 2);
  auto* src = static_cast<int64_t*>(duckdb_vector_get_data(src_vec));
  auto* dst = static_cast<int64_t*>(duckdb_vector_get_data(dst_vec));
  auto* root = static_cast<int64_t*>(duckdb_vector_get_data(root_vec));
  uint64_t* src_valid = duckdb_vector_get_validity(src_vec);
  uint64_t* dst_valid = duckdb_vector_get_validity(dst_vec);
  uint64_t* root_valid = duckdb_vector_get_validity(root_vec);
  for (idx_t row = 0; row < rows; ++row) {
    AggState& slot = *reinterpret_cast<AggState*>(states[row]);
    if (!slot) {
      slot = new DomBuffer();
    }
    // The root is a constant expression (same for every row); capture the
    // first non-null value seen.
    if (!slot->root &&
        !(root_valid && !duckdb_validity_row_is_valid(root_valid, row))) {
      slot->root = root[row];
    }
    if ((src_valid && !duckdb_validity_row_is_valid(src_valid, row)) ||
        (dst_valid && !duckdb_validity_row_is_valid(dst_valid, row))) {
      continue;  // NULL endpoints do not form an edge.
    }
    slot->srcs.push_back(src[row]);
    slot->dsts.push_back(dst[row]);
  }
}

void Combine(duckdb_function_info,
             duckdb_aggregate_state* source,
             duckdb_aggregate_state* target,
             idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    AggState src = *reinterpret_cast<AggState*>(source[i]);
    if (!src) {
      continue;
    }
    AggState& dst = *reinterpret_cast<AggState*>(target[i]);
    if (!dst) {
      dst = new DomBuffer();
    }
    dst->srcs.insert(dst->srcs.end(), src->srcs.begin(), src->srcs.end());
    dst->dsts.insert(dst->dsts.end(), src->dsts.begin(), src->dsts.end());
    if (!dst->root) {
      dst->root = src->root;
    }
  }
}

void Finalize(duckdb_function_info,
              duckdb_aggregate_state* source,
              duckdb_vector result,
              idx_t count,
              idx_t offset) {
  std::vector<std::vector<ResultRow>> per_group(count);
  idx_t total = 0;
  for (idx_t i = 0; i < count; ++i) {
    AggState& slot = *reinterpret_cast<AggState*>(source[i]);
    std::unique_ptr<DomBuffer> buf(slot ? slot : new DomBuffer());
    slot = nullptr;
    per_group[i] = RunDominator(*buf);
    total += per_group[i].size();
  }

  duckdb_list_vector_reserve(result, total);
  duckdb_list_vector_set_size(result, total);
  duckdb_vector struct_vec = duckdb_list_vector_get_child(result);
  duckdb_vector node_child = duckdb_struct_vector_get_child(struct_vec, 0);
  duckdb_vector dom_child = duckdb_struct_vector_get_child(struct_vec, 1);
  auto* node_out = static_cast<int64_t*>(duckdb_vector_get_data(node_child));
  auto* dom_out = static_cast<int64_t*>(duckdb_vector_get_data(dom_child));
  duckdb_vector_ensure_validity_writable(dom_child);
  uint64_t* dom_valid = duckdb_vector_get_validity(dom_child);

  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(result));
  idx_t cursor = 0;
  for (idx_t i = 0; i < count; ++i) {
    entries[offset + i].offset = cursor;
    entries[offset + i].length = per_group[i].size();
    for (const ResultRow& r : per_group[i]) {
      node_out[cursor] = r.node_id;
      if (r.has_dominator) {
        dom_out[cursor] = r.dominator_id;
        duckdb_validity_set_row_valid(dom_valid, cursor);
      } else {
        duckdb_validity_set_row_invalid(dom_valid, cursor);
      }
      ++cursor;
    }
  }
}

void Destroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    AggState& slot = *reinterpret_cast<AggState*>(states[i]);
    delete slot;
    slot = nullptr;
  }
}

}  // namespace

base::Status RegisterDominatorTree(duckdb_connection conn) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, "__intrinsic_dominator_tree");
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_aggregate_function_add_parameter(f, bigint);  // source_node_id
  duckdb_aggregate_function_add_parameter(f, bigint);  // dest_node_id
  duckdb_aggregate_function_add_parameter(f, bigint);  // root_node_id

  duckdb_logical_type members[2] = {bigint, bigint};
  const char* names[2] = {"node_id", "dominator_node_id"};
  duckdb_logical_type struct_type =
      duckdb_create_struct_type(members, names, 2);
  duckdb_logical_type list_of_struct = duckdb_create_list_type(struct_type);
  duckdb_aggregate_function_set_return_type(f, list_of_struct);

  duckdb_aggregate_function_set_functions(f, StateSize, Init, Update, Combine,
                                          Finalize);
  duckdb_aggregate_function_set_destructor(f, Destroy);
  duckdb_state st = duckdb_register_aggregate_function(conn, f);

  duckdb_destroy_logical_type(&list_of_struct);
  duckdb_destroy_logical_type(&struct_type);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_aggregate_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterDominatorTree: registration failed");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
