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

#include "src/trace_processor/duckdb/graph_function.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/duckdb/udf_handle_registry.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Collected directed edges of a graph, in scan order (source -> dest).
struct GraphEdges {
  std::vector<int64_t> srcs;
  std::vector<int64_t> dsts;
};

// Collected list of node ids (BFS/DFS start nodes), in scan order.
struct IntArray {
  std::vector<int64_t> vals;
};

// A result row of a reachability traversal.
struct ResultRow {
  int64_t node_id;
  int64_t parent_id;
  bool has_parent;
};

// Collected weighted edges (source, dest, edge_weight) for weight-bounded DFS.
struct WeightedGraphEdges {
  std::vector<int64_t> srcs;
  std::vector<int64_t> dsts;
  std::vector<int64_t> weights;
};

// Collected roots (root_node_id, root_target_weight) for weight-bounded DFS.
struct WeightedRoots {
  std::vector<int64_t> ids;
  std::vector<int64_t> weights;
};

// A result row of weight-bounded DFS: (root_node_id, node_id, parent_node_id).
struct WResultRow {
  int64_t root_node_id;
  int64_t node_id;
  int64_t parent_id;
  bool has_parent;
};

// === Aggregate boilerplate ==================================================
// Each aggregate state is a single pointer to a heap buffer (or null), matching
// the interval_intersect port. finalize() hands the buffer to the per-type
// HandleRegistry and returns the int64 handle.

template <typename T>
idx_t BufStateSize(duckdb_function_info) {
  return sizeof(T*);
}

template <typename T>
void BufInit(duckdb_function_info, duckdb_aggregate_state state) {
  *reinterpret_cast<T**>(state) = nullptr;
}

template <typename T>
void BufCombine(duckdb_function_info,
                duckdb_aggregate_state* source,
                duckdb_aggregate_state* target,
                idx_t count,
                void (*merge)(T*, T*)) {
  for (idx_t i = 0; i < count; ++i) {
    T* src = *reinterpret_cast<T**>(source[i]);
    if (!src) {
      continue;
    }
    T*& dst = *reinterpret_cast<T**>(target[i]);
    if (!dst) {
      dst = new T();
    }
    merge(src, dst);
  }
}

template <typename T>
void BufFinalize(duckdb_function_info,
                 duckdb_aggregate_state* source,
                 duckdb_vector result,
                 idx_t count,
                 idx_t offset) {
  auto* out = static_cast<int64_t*>(duckdb_vector_get_data(result));
  for (idx_t i = 0; i < count; ++i) {
    T*& slot = *reinterpret_cast<T**>(source[i]);
    std::unique_ptr<T> buf(slot ? slot : new T());
    slot = nullptr;
    out[offset + i] = HandleRegistry<T>::Instance().Insert(std::move(buf));
  }
}

template <typename T>
void BufDestroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    T*& slot = *reinterpret_cast<T**>(states[i]);
    delete slot;
    slot = nullptr;
  }
}

// --- __intrinsic_graph_agg(source_node_id, dest_node_id) -> BIGINT -----------

void GraphAggUpdate(duckdb_function_info,
                    duckdb_data_chunk input,
                    duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector src_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector dst_vec = duckdb_data_chunk_get_vector(input, 1);
  auto* src = static_cast<int64_t*>(duckdb_vector_get_data(src_vec));
  auto* dst = static_cast<int64_t*>(duckdb_vector_get_data(dst_vec));
  uint64_t* src_valid = duckdb_vector_get_validity(src_vec);
  uint64_t* dst_valid = duckdb_vector_get_validity(dst_vec);
  for (idx_t row = 0; row < rows; ++row) {
    // NULL endpoints do not form an edge (the SQLite path filters them).
    if ((src_valid && !duckdb_validity_row_is_valid(src_valid, row)) ||
        (dst_valid && !duckdb_validity_row_is_valid(dst_valid, row))) {
      continue;
    }
    GraphEdges*& slot = *reinterpret_cast<GraphEdges**>(states[row]);
    if (!slot) {
      slot = new GraphEdges();
    }
    slot->srcs.push_back(src[row]);
    slot->dsts.push_back(dst[row]);
  }
}

void GraphAggCombine(duckdb_function_info info,
                     duckdb_aggregate_state* source,
                     duckdb_aggregate_state* target,
                     idx_t count) {
  BufCombine<GraphEdges>(info, source, target, count, [](GraphEdges* s,
                                                         GraphEdges* d) {
    d->srcs.insert(d->srcs.end(), s->srcs.begin(), s->srcs.end());
    d->dsts.insert(d->dsts.end(), s->dsts.begin(), s->dsts.end());
  });
}

// --- __intrinsic_int_array_agg(node_id) -> BIGINT ---------------------------

void IntArrayUpdate(duckdb_function_info,
                    duckdb_data_chunk input,
                    duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector vec = duckdb_data_chunk_get_vector(input, 0);
  auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vec));
  uint64_t* valid = duckdb_vector_get_validity(vec);
  for (idx_t row = 0; row < rows; ++row) {
    if (valid && !duckdb_validity_row_is_valid(valid, row)) {
      continue;
    }
    IntArray*& slot = *reinterpret_cast<IntArray**>(states[row]);
    if (!slot) {
      slot = new IntArray();
    }
    slot->vals.push_back(data[row]);
  }
}

void IntArrayCombine(duckdb_function_info info,
                     duckdb_aggregate_state* source,
                     duckdb_aggregate_state* target,
                     idx_t count) {
  BufCombine<IntArray>(info, source, target, count,
                       [](IntArray* s, IntArray* d) {
                         d->vals.insert(d->vals.end(), s->vals.begin(),
                                        s->vals.end());
                       });
}

// === BFS/DFS reachability (replicated from the graph_traversal plugin) =======

// Builds adjacency (indexed by node id, sized to max(source,dest)+1 so that
// dest-only nodes are valid leaf entries), exactly like __intrinsic_graph_agg.
std::vector<std::vector<uint32_t>> BuildAdjacency(const GraphEdges& edges,
                                                  uint32_t* graph_size_out) {
  uint32_t graph_size = 0;
  for (size_t i = 0; i < edges.srcs.size(); ++i) {
    graph_size = std::max(graph_size,
                          static_cast<uint32_t>(edges.srcs[i]) + 1);
    graph_size = std::max(graph_size,
                          static_cast<uint32_t>(edges.dsts[i]) + 1);
  }
  std::vector<std::vector<uint32_t>> adj(graph_size);
  for (size_t i = 0; i < edges.srcs.size(); ++i) {
    adj[static_cast<uint32_t>(edges.srcs[i])].push_back(
        static_cast<uint32_t>(edges.dsts[i]));
  }
  *graph_size_out = graph_size;
  return adj;
}

std::vector<ResultRow> RunDfs(const GraphEdges& edges,
                              const std::vector<int64_t>& starts) {
  std::vector<ResultRow> out;
  if (edges.srcs.empty() || starts.empty()) {
    return out;  // Matches the plugin: null graph/array -> empty result.
  }
  uint32_t graph_size = 0;
  std::vector<std::vector<uint32_t>> adj = BuildAdjacency(edges, &graph_size);
  uint32_t max_id = graph_size;
  for (int64_t s : starts) {
    max_id = std::max(max_id, static_cast<uint32_t>(s) + 1);
  }
  std::vector<bool> visited(max_id);
  struct State {
    uint32_t id;
    std::optional<uint32_t> parent;
  };
  std::vector<State> stack;
  for (int64_t s : starts) {
    stack.push_back(State{static_cast<uint32_t>(s), std::nullopt});
  }
  while (!stack.empty()) {
    State st = stack.back();
    stack.pop_back();
    if (visited[st.id]) {
      continue;
    }
    out.push_back(ResultRow{st.id, st.parent ? *st.parent : 0,
                            st.parent.has_value()});
    visited[st.id] = true;
    if (st.id >= graph_size) {
      continue;
    }
    const auto& children = adj[st.id];
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(State{*it, st.id});
    }
  }
  return out;
}

std::vector<ResultRow> RunBfs(const GraphEdges& edges,
                              const std::vector<int64_t>& starts) {
  std::vector<ResultRow> out;
  if (edges.srcs.empty() || starts.empty()) {
    return out;
  }
  uint32_t graph_size = 0;
  std::vector<std::vector<uint32_t>> adj = BuildAdjacency(edges, &graph_size);
  uint32_t max_id = graph_size;
  for (int64_t s : starts) {
    max_id = std::max(max_id, static_cast<uint32_t>(s) + 1);
  }
  std::vector<bool> visited(max_id);
  struct State {
    uint32_t id;
    std::optional<uint32_t> parent;
  };
  std::deque<State> queue;
  for (int64_t s : starts) {
    auto id = static_cast<uint32_t>(s);
    if (visited[id]) {
      continue;
    }
    visited[id] = true;
    queue.push_back(State{id, std::nullopt});
  }
  while (!queue.empty()) {
    State st = queue.front();
    queue.pop_front();
    out.push_back(ResultRow{st.id, st.parent ? *st.parent : 0,
                            st.parent.has_value()});
    if (st.id >= graph_size) {
      continue;
    }
    for (uint32_t n : adj[st.id]) {
      if (visited[n]) {
        continue;
      }
      visited[n] = true;
      queue.push_back(State{n, st.id});
    }
  }
  return out;
}

// === Scalar combiners =======================================================
// __intrinsic_graph_{bfs,dfs}(graph_handle, starts_handle)
//   -> LIST<STRUCT(node_id BIGINT, parent_node_id BIGINT)>

void CombineImpl(duckdb_function_info info,
                 duckdb_data_chunk input,
                 duckdb_vector output,
                 bool is_bfs) {
  idx_t out_rows = duckdb_data_chunk_get_size(input);
  duckdb_vector gh_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector sh_vec = duckdb_data_chunk_get_vector(input, 1);
  auto* gh = static_cast<int64_t*>(duckdb_vector_get_data(gh_vec));
  auto* sh = static_cast<int64_t*>(duckdb_vector_get_data(sh_vec));
  uint64_t* gh_valid = duckdb_vector_get_validity(gh_vec);
  uint64_t* sh_valid = duckdb_vector_get_validity(sh_vec);

  std::vector<std::vector<ResultRow>> per_row(out_rows);
  idx_t total = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    if ((gh_valid && !duckdb_validity_row_is_valid(gh_valid, r)) ||
        (sh_valid && !duckdb_validity_row_is_valid(sh_valid, r))) {
      continue;
    }
    std::unique_ptr<GraphEdges> edges =
        HandleRegistry<GraphEdges>::Instance().Take(gh[r]);
    std::unique_ptr<IntArray> starts =
        HandleRegistry<IntArray>::Instance().Take(sh[r]);
    if (!edges || !starts) {
      continue;
    }
    per_row[r] = is_bfs ? RunBfs(*edges, starts->vals)
                        : RunDfs(*edges, starts->vals);
    total += per_row[r].size();
  }

  if (duckdb_list_vector_reserve(output, total) == DuckDBError) {
    duckdb_scalar_function_set_error(info, "graph combine: reserve failed");
    return;
  }
  duckdb_list_vector_set_size(output, total);
  duckdb_vector struct_vec = duckdb_list_vector_get_child(output);
  duckdb_vector node_child = duckdb_struct_vector_get_child(struct_vec, 0);
  duckdb_vector parent_child = duckdb_struct_vector_get_child(struct_vec, 1);
  auto* node_out = static_cast<int64_t*>(duckdb_vector_get_data(node_child));
  auto* parent_out =
      static_cast<int64_t*>(duckdb_vector_get_data(parent_child));
  duckdb_vector_ensure_validity_writable(parent_child);
  uint64_t* parent_valid = duckdb_vector_get_validity(parent_child);

  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output));
  idx_t cursor = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    entries[r].offset = cursor;
    entries[r].length = per_row[r].size();
    for (const ResultRow& row : per_row[r]) {
      node_out[cursor] = row.node_id;
      if (row.has_parent) {
        parent_out[cursor] = row.parent_id;
        duckdb_validity_set_row_valid(parent_valid, cursor);
      } else {
        duckdb_validity_set_row_invalid(parent_valid, cursor);
      }
      ++cursor;
    }
  }
}

void CombineBfs(duckdb_function_info info,
                duckdb_data_chunk input,
                duckdb_vector output) {
  CombineImpl(info, input, output, /*is_bfs=*/true);
}

void CombineDfs(duckdb_function_info info,
                duckdb_data_chunk input,
                duckdb_vector output) {
  CombineImpl(info, input, output, /*is_bfs=*/false);
}

// === Weight-bounded DFS (replicated from the dfs_weight_bounded plugin) ======

void WGraphAggUpdate(duckdb_function_info,
                     duckdb_data_chunk input,
                     duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector s_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector d_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector w_vec = duckdb_data_chunk_get_vector(input, 2);
  auto* s = static_cast<int64_t*>(duckdb_vector_get_data(s_vec));
  auto* d = static_cast<int64_t*>(duckdb_vector_get_data(d_vec));
  auto* w = static_cast<int64_t*>(duckdb_vector_get_data(w_vec));
  uint64_t* sv = duckdb_vector_get_validity(s_vec);
  uint64_t* dv = duckdb_vector_get_validity(d_vec);
  for (idx_t row = 0; row < rows; ++row) {
    if ((sv && !duckdb_validity_row_is_valid(sv, row)) ||
        (dv && !duckdb_validity_row_is_valid(dv, row))) {
      continue;
    }
    WeightedGraphEdges*& slot =
        *reinterpret_cast<WeightedGraphEdges**>(states[row]);
    if (!slot) {
      slot = new WeightedGraphEdges();
    }
    slot->srcs.push_back(s[row]);
    slot->dsts.push_back(d[row]);
    slot->weights.push_back(w[row]);
  }
}

void WGraphAggCombine(duckdb_function_info info,
                      duckdb_aggregate_state* source,
                      duckdb_aggregate_state* target,
                      idx_t count) {
  BufCombine<WeightedGraphEdges>(
      info, source, target, count, [](WeightedGraphEdges* s,
                                      WeightedGraphEdges* d) {
        d->srcs.insert(d->srcs.end(), s->srcs.begin(), s->srcs.end());
        d->dsts.insert(d->dsts.end(), s->dsts.begin(), s->dsts.end());
        d->weights.insert(d->weights.end(), s->weights.begin(),
                          s->weights.end());
      });
}

void RootWeightAggUpdate(duckdb_function_info,
                         duckdb_data_chunk input,
                         duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector id_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector w_vec = duckdb_data_chunk_get_vector(input, 1);
  auto* id = static_cast<int64_t*>(duckdb_vector_get_data(id_vec));
  auto* w = static_cast<int64_t*>(duckdb_vector_get_data(w_vec));
  uint64_t* idv = duckdb_vector_get_validity(id_vec);
  for (idx_t row = 0; row < rows; ++row) {
    if (idv && !duckdb_validity_row_is_valid(idv, row)) {
      continue;
    }
    WeightedRoots*& slot = *reinterpret_cast<WeightedRoots**>(states[row]);
    if (!slot) {
      slot = new WeightedRoots();
    }
    slot->ids.push_back(id[row]);
    slot->weights.push_back(w[row]);
  }
}

void RootWeightAggCombine(duckdb_function_info info,
                          duckdb_aggregate_state* source,
                          duckdb_aggregate_state* target,
                          idx_t count) {
  BufCombine<WeightedRoots>(info, source, target, count,
                            [](WeightedRoots* s, WeightedRoots* d) {
                              d->ids.insert(d->ids.end(), s->ids.begin(),
                                            s->ids.end());
                              d->weights.insert(d->weights.end(),
                                                s->weights.begin(),
                                                s->weights.end());
                            });
}

// Verbatim port of DfsWeightBoundedImpl (dfs_weight_bounded.cc): iterative DFS
// from each root, accumulating edge weights along the path; stops at the target
// weight (floor: include the node that crossed it; ceiling: exclude it).
std::vector<WResultRow> RunDfsWeightBounded(const WeightedGraphEdges& g,
                                            const WeightedRoots& r,
                                            bool is_target_weight_floor) {
  std::vector<WResultRow> out;
  if (g.srcs.empty() || r.ids.empty()) {
    return out;  // Matches the plugin: null graph/roots -> empty.
  }
  struct WEdge {
    uint32_t id;
    uint32_t weight;
  };
  uint32_t size = 0;
  for (size_t i = 0; i < g.srcs.size(); ++i) {
    size = std::max(size, static_cast<uint32_t>(g.srcs[i]) + 1);
    size = std::max(size, static_cast<uint32_t>(g.dsts[i]) + 1);
  }
  for (int64_t id : r.ids) {
    size = std::max(size, static_cast<uint32_t>(id) + 1);
  }
  std::vector<std::vector<WEdge>> adj(size);
  for (size_t i = 0; i < g.srcs.size(); ++i) {
    adj[static_cast<uint32_t>(g.srcs[i])].push_back(
        {static_cast<uint32_t>(g.dsts[i]),
         static_cast<uint32_t>(g.weights[i])});
  }
  struct StackState {
    uint32_t id;
    uint32_t weight;
    std::optional<uint32_t> parent_id;
  };
  std::vector<uint8_t> seen(size);
  std::vector<StackState> stack;
  for (size_t k = 0; k < r.ids.size(); ++k) {
    auto root_id = static_cast<uint32_t>(r.ids[k]);
    auto root_weight = static_cast<uint32_t>(r.weights[k]);
    stack.clear();
    stack.push_back({root_id, 0, std::nullopt});
    std::fill(seen.begin(), seen.end(), 0);
    for (uint32_t total_weight = 0; !stack.empty();) {
      StackState st = stack.back();
      stack.pop_back();
      if (seen[st.id]) {
        continue;
      }
      seen[st.id] = true;
      total_weight += st.weight;
      if (!is_target_weight_floor && total_weight > root_weight) {
        break;  // Ceiling: don't include the node that crosses the threshold.
      }
      WResultRow row;
      row.root_node_id = static_cast<int64_t>(root_id);
      row.node_id = static_cast<int64_t>(st.id);
      row.has_parent = st.parent_id.has_value();
      row.parent_id = row.has_parent ? static_cast<int64_t>(*st.parent_id) : 0;
      out.push_back(row);
      if (total_weight > root_weight) {
        break;  // Floor: include the crossing node, then stop.
      }
      const auto& children = adj[st.id];
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        stack.push_back({it->id, it->weight, st.id});
      }
    }
  }
  return out;
}

// __intrinsic_dfs_weight_bounded(graph_handle, roots_handle, is_floor BOOLEAN)
//   -> LIST<STRUCT(root_node_id BIGINT, node_id BIGINT, parent_node_id BIGINT)>
void CombineDfsWeightBounded(duckdb_function_info info,
                             duckdb_data_chunk input,
                             duckdb_vector output) {
  idx_t out_rows = duckdb_data_chunk_get_size(input);
  duckdb_vector gh_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector rh_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector fl_vec = duckdb_data_chunk_get_vector(input, 2);
  auto* gh = static_cast<int64_t*>(duckdb_vector_get_data(gh_vec));
  auto* rh = static_cast<int64_t*>(duckdb_vector_get_data(rh_vec));
  auto* fl = static_cast<int64_t*>(duckdb_vector_get_data(fl_vec));
  uint64_t* gh_valid = duckdb_vector_get_validity(gh_vec);
  uint64_t* rh_valid = duckdb_vector_get_validity(rh_vec);

  std::vector<std::vector<WResultRow>> per_row(out_rows);
  idx_t total = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    if ((gh_valid && !duckdb_validity_row_is_valid(gh_valid, r)) ||
        (rh_valid && !duckdb_validity_row_is_valid(rh_valid, r))) {
      continue;
    }
    std::unique_ptr<WeightedGraphEdges> g =
        HandleRegistry<WeightedGraphEdges>::Instance().Take(gh[r]);
    std::unique_ptr<WeightedRoots> roots =
        HandleRegistry<WeightedRoots>::Instance().Take(rh[r]);
    if (!g || !roots) {
      continue;
    }
    per_row[r] = RunDfsWeightBounded(*g, *roots, fl[r] != 0);
    total += per_row[r].size();
  }

  if (duckdb_list_vector_reserve(output, total) == DuckDBError) {
    duckdb_scalar_function_set_error(info, "dfs_weight_bounded: reserve failed");
    return;
  }
  duckdb_list_vector_set_size(output, total);
  duckdb_vector sv = duckdb_list_vector_get_child(output);
  duckdb_vector c_root = duckdb_struct_vector_get_child(sv, 0);
  duckdb_vector c_node = duckdb_struct_vector_get_child(sv, 1);
  duckdb_vector c_parent = duckdb_struct_vector_get_child(sv, 2);
  auto* root_out = static_cast<int64_t*>(duckdb_vector_get_data(c_root));
  auto* node_out = static_cast<int64_t*>(duckdb_vector_get_data(c_node));
  auto* parent_out = static_cast<int64_t*>(duckdb_vector_get_data(c_parent));
  duckdb_vector_ensure_validity_writable(c_parent);
  uint64_t* parent_valid = duckdb_vector_get_validity(c_parent);

  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output));
  idx_t cursor = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    entries[r].offset = cursor;
    entries[r].length = per_row[r].size();
    for (const WResultRow& row : per_row[r]) {
      root_out[cursor] = row.root_node_id;
      node_out[cursor] = row.node_id;
      if (row.has_parent) {
        parent_out[cursor] = row.parent_id;
        duckdb_validity_set_row_valid(parent_valid, cursor);
      } else {
        duckdb_validity_set_row_invalid(parent_valid, cursor);
      }
      ++cursor;
    }
  }
}

// === Registration ===========================================================

base::Status RegisterAgg(duckdb_connection conn,
                         const char* name,
                         int n_params,
                         duckdb_aggregate_update_t update,
                         duckdb_aggregate_combine_t combine,
                         duckdb_aggregate_state_size state_size,
                         duckdb_aggregate_init_t init,
                         duckdb_aggregate_finalize_t finalize,
                         duckdb_aggregate_destroy_t destroy) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, name);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  for (int i = 0; i < n_params; ++i) {
    duckdb_aggregate_function_add_parameter(f, bigint);
  }
  duckdb_aggregate_function_set_return_type(f, bigint);
  duckdb_destroy_logical_type(&bigint);
  duckdb_aggregate_function_set_functions(f, state_size, init, update, combine,
                                          finalize);
  duckdb_aggregate_function_set_destructor(f, destroy);
  duckdb_state st = duckdb_register_aggregate_function(conn, f);
  duckdb_destroy_aggregate_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterGraphFunctions: agg '%s' failed", name);
  }
  return base::OkStatus();
}

base::Status RegisterCombiner(duckdb_connection conn,
                              const char* name,
                              duckdb_scalar_function_t fn) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_scalar_function_add_parameter(f, bigint);  // graph handle
  duckdb_scalar_function_add_parameter(f, bigint);  // starts handle

  duckdb_logical_type members[2] = {bigint, bigint};
  const char* names[2] = {"node_id", "parent_node_id"};
  duckdb_logical_type struct_type =
      duckdb_create_struct_type(members, names, 2);
  duckdb_logical_type list_of_struct = duckdb_create_list_type(struct_type);
  duckdb_scalar_function_set_return_type(f, list_of_struct);
  duckdb_scalar_function_set_function(f, fn);
  duckdb_state st = duckdb_register_scalar_function(conn, f);

  duckdb_destroy_logical_type(&list_of_struct);
  duckdb_destroy_logical_type(&struct_type);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterGraphFunctions: combiner '%s' failed",
                           name);
  }
  return base::OkStatus();
}

}  // namespace

base::Status RegisterGraphFunctions(duckdb_connection conn) {
  RETURN_IF_ERROR(RegisterAgg(
      conn, "__intrinsic_graph_agg", 2, GraphAggUpdate, GraphAggCombine,
      BufStateSize<GraphEdges>, BufInit<GraphEdges>, BufFinalize<GraphEdges>,
      BufDestroy<GraphEdges>));
  RETURN_IF_ERROR(RegisterAgg(
      conn, "__intrinsic_int_array_agg", 1, IntArrayUpdate, IntArrayCombine,
      BufStateSize<IntArray>, BufInit<IntArray>, BufFinalize<IntArray>,
      BufDestroy<IntArray>));
  RETURN_IF_ERROR(RegisterCombiner(conn, "__intrinsic_graph_bfs", CombineBfs));
  RETURN_IF_ERROR(RegisterCombiner(conn, "__intrinsic_graph_dfs", CombineDfs));

  // Weight-bounded DFS: a weighted-edge aggregate, a (root_id, target_weight)
  // aggregate, and the combiner (graph_handle, roots_handle, is_floor) ->
  // LIST<STRUCT(root_node_id, node_id, parent_node_id)>.
  RETURN_IF_ERROR(RegisterAgg(
      conn, "__intrinsic_wgraph_agg", 3, WGraphAggUpdate, WGraphAggCombine,
      BufStateSize<WeightedGraphEdges>, BufInit<WeightedGraphEdges>,
      BufFinalize<WeightedGraphEdges>, BufDestroy<WeightedGraphEdges>));
  RETURN_IF_ERROR(RegisterAgg(
      conn, "__intrinsic_root_weight_agg", 2, RootWeightAggUpdate,
      RootWeightAggCombine, BufStateSize<WeightedRoots>, BufInit<WeightedRoots>,
      BufFinalize<WeightedRoots>, BufDestroy<WeightedRoots>));
  {
    duckdb_scalar_function f = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(f, "__intrinsic_dfs_weight_bounded");
    duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    duckdb_scalar_function_add_parameter(f, bigint);  // graph handle
    duckdb_scalar_function_add_parameter(f, bigint);  // roots handle
    // is_target_weight_floor: BIGINT (the macro passes 0/1; non-zero = floor) so
    // an integer literal binds without a cast.
    duckdb_scalar_function_add_parameter(f, bigint);
    duckdb_logical_type members[3] = {bigint, bigint, bigint};
    const char* names[3] = {"root_node_id", "node_id", "parent_node_id"};
    duckdb_logical_type struct_type =
        duckdb_create_struct_type(members, names, 3);
    duckdb_logical_type list_of_struct = duckdb_create_list_type(struct_type);
    duckdb_scalar_function_set_return_type(f, list_of_struct);
    duckdb_scalar_function_set_function(f, CombineDfsWeightBounded);
    duckdb_state st = duckdb_register_scalar_function(conn, f);
    duckdb_destroy_logical_type(&list_of_struct);
    duckdb_destroy_logical_type(&struct_type);
    duckdb_destroy_logical_type(&bigint);
    duckdb_destroy_scalar_function(&f);
    if (st == DuckDBError) {
      return base::ErrStatus(
          "RegisterGraphFunctions: __intrinsic_dfs_weight_bounded failed");
    }
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
