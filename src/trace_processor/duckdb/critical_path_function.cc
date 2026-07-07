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

#include "src/trace_processor/duckdb/critical_path_function.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/duckdb/udf_handle_registry.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// === WakeupGraph + the critical-path walk (verbatim port of the
// critical_path plugin; the algorithm is plain interval arithmetic). ==========

struct WakeupNode {
  uint32_t utid = 0;
  int64_t ts = 0;
  int64_t dur = 0;
  std::optional<int64_t> idle_dur;
  std::optional<uint32_t> waker_id;
  std::optional<uint32_t> prev_id;
};

struct WakeupGraph {
  std::vector<std::optional<WakeupNode>> nodes_by_id;
};

struct RootArray {
  std::vector<int64_t> ids;
};

struct ResultRow {
  int64_t root_id;
  int64_t depth;
  int64_t ts;
  int64_t dur;
  int64_t blocker_id;
  int64_t blocker_utid;
  int64_t parent_id;
};

struct Frame {
  uint32_t node_id;
  int64_t window_start;
  int64_t window_end;
  uint32_t depth;
  uint32_t parent_node_id;
};

void WalkOneRoot(const WakeupGraph& graph,
                 uint32_t root_id,
                 std::vector<ResultRow>& out,
                 std::vector<Frame>& stack) {
  if (root_id >= graph.nodes_by_id.size() || !graph.nodes_by_id[root_id]) {
    return;
  }
  const WakeupNode& root = *graph.nodes_by_id[root_id];
  stack.clear();
  int64_t initial_start = root.ts - root.idle_dur.value_or(0);
  int64_t initial_end = root.ts + root.dur;
  stack.push_back({root_id, initial_start, initial_end, 0, root_id});

  while (!stack.empty()) {
    Frame f = stack.back();
    stack.pop_back();

    if (f.window_start >= f.window_end ||
        f.node_id >= graph.nodes_by_id.size() ||
        !graph.nodes_by_id[f.node_id]) {
      continue;
    }
    const WakeupNode& n = *graph.nodes_by_id[f.node_id];
    int64_t node_idle_start =
        n.idle_dur.has_value() ? (n.ts - *n.idle_dur) : f.window_start;
    int64_t node_run_end = n.ts + n.dur;
    int64_t eff_start = std::max(f.window_start, node_idle_start);
    int64_t eff_end = std::min(f.window_end, node_run_end);

    if (n.idle_dur.has_value() && f.window_start < node_idle_start &&
        n.prev_id) {
      int64_t prev_window_end = std::min(f.window_end, node_idle_start);
      stack.push_back({*n.prev_id, f.window_start, prev_window_end, f.depth,
                       f.parent_node_id});
    }
    if (eff_start >= eff_end) {
      continue;
    }
    int64_t idle_clip_start = eff_start;
    int64_t idle_clip_end = std::min(eff_end, n.ts);
    if (idle_clip_start < idle_clip_end) {
      if (!n.waker_id && n.prev_id) {
        out.push_back(ResultRow{root_id, f.depth, idle_clip_start,
                                idle_clip_end - idle_clip_start, f.node_id,
                                n.utid, f.parent_node_id});
      } else if (n.waker_id) {
        stack.push_back({*n.waker_id, idle_clip_start, idle_clip_end,
                         f.depth + 1, f.parent_node_id});
      }
    }
    int64_t run_start = std::max(eff_start, n.ts);
    if (run_start < eff_end) {
      out.push_back(ResultRow{root_id, f.depth, run_start, eff_end - run_start,
                              f.node_id, n.utid, f.parent_node_id});
    }
  }
}

// === Aggregate boilerplate (one heap buffer per state). ======================

template <typename T>
idx_t BufStateSize(duckdb_function_info) {
  return sizeof(T*);
}
template <typename T>
void BufInit(duckdb_function_info, duckdb_aggregate_state state) {
  *reinterpret_cast<T**>(state) = nullptr;
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

// __intrinsic_wakeup_graph_agg(id, utid, ts, dur, idle_dur, waker_id, prev_id).
void GraphUpdate(duckdb_function_info,
                 duckdb_data_chunk input,
                 duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector v[7];
  int64_t* d[7];
  uint64_t* valid[7];
  for (int c = 0; c < 7; ++c) {
    v[c] = duckdb_data_chunk_get_vector(input, static_cast<idx_t>(c));
    d[c] = static_cast<int64_t*>(duckdb_vector_get_data(v[c]));
    valid[c] = duckdb_vector_get_validity(v[c]);
  }
  auto is_null = [&](int c, idx_t row) {
    return valid[c] && !duckdb_validity_row_is_valid(valid[c], row);
  };
  for (idx_t row = 0; row < rows; ++row) {
    if (is_null(0, row)) {
      continue;  // A NULL id cannot index a node.
    }
    WakeupGraph*& slot = *reinterpret_cast<WakeupGraph**>(states[row]);
    if (!slot) {
      slot = new WakeupGraph();
    }
    auto id = static_cast<uint32_t>(d[0][row]);
    if (id >= slot->nodes_by_id.size()) {
      slot->nodes_by_id.resize(id + 1);
    }
    WakeupNode n;
    n.utid = static_cast<uint32_t>(d[1][row]);
    n.ts = d[2][row];
    n.dur = d[3][row];
    if (!is_null(4, row)) {
      n.idle_dur = d[4][row];
    }
    if (!is_null(5, row)) {
      n.waker_id = static_cast<uint32_t>(d[5][row]);
    }
    if (!is_null(6, row)) {
      n.prev_id = static_cast<uint32_t>(d[6][row]);
    }
    slot->nodes_by_id[id] = n;
  }
}

void GraphCombine(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_aggregate_state* target,
                  idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    WakeupGraph* src = *reinterpret_cast<WakeupGraph**>(source[i]);
    if (!src) {
      continue;
    }
    WakeupGraph*& dst = *reinterpret_cast<WakeupGraph**>(target[i]);
    if (!dst) {
      dst = new WakeupGraph();
    }
    if (dst->nodes_by_id.size() < src->nodes_by_id.size()) {
      dst->nodes_by_id.resize(src->nodes_by_id.size());
    }
    for (size_t k = 0; k < src->nodes_by_id.size(); ++k) {
      if (src->nodes_by_id[k]) {
        dst->nodes_by_id[k] = src->nodes_by_id[k];
      }
    }
  }
}

// __intrinsic_cp_roots_agg(root_id).
void RootsUpdate(duckdb_function_info,
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
    RootArray*& slot = *reinterpret_cast<RootArray**>(states[row]);
    if (!slot) {
      slot = new RootArray();
    }
    slot->ids.push_back(data[row]);
  }
}

void RootsCombine(duckdb_function_info,
                  duckdb_aggregate_state* source,
                  duckdb_aggregate_state* target,
                  idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    RootArray* src = *reinterpret_cast<RootArray**>(source[i]);
    if (!src) {
      continue;
    }
    RootArray*& dst = *reinterpret_cast<RootArray**>(target[i]);
    if (!dst) {
      dst = new RootArray();
    }
    dst->ids.insert(dst->ids.end(), src->ids.begin(), src->ids.end());
  }
}

// __intrinsic_critical_path_walk(graph_handle, roots_handle) -> LIST<STRUCT>.
void CombineWalk(duckdb_function_info info,
                 duckdb_data_chunk input,
                 duckdb_vector output) {
  idx_t out_rows = duckdb_data_chunk_get_size(input);
  duckdb_vector gh_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector rh_vec = duckdb_data_chunk_get_vector(input, 1);
  auto* gh = static_cast<int64_t*>(duckdb_vector_get_data(gh_vec));
  auto* rh = static_cast<int64_t*>(duckdb_vector_get_data(rh_vec));
  uint64_t* gh_valid = duckdb_vector_get_validity(gh_vec);
  uint64_t* rh_valid = duckdb_vector_get_validity(rh_vec);

  std::vector<std::vector<ResultRow>> per_row(out_rows);
  idx_t total = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    if ((gh_valid && !duckdb_validity_row_is_valid(gh_valid, r)) ||
        (rh_valid && !duckdb_validity_row_is_valid(rh_valid, r))) {
      continue;
    }
    std::unique_ptr<WakeupGraph> graph =
        HandleRegistry<WakeupGraph>::Instance().Take(gh[r]);
    std::unique_ptr<RootArray> roots =
        HandleRegistry<RootArray>::Instance().Take(rh[r]);
    if (!graph || !roots || graph->nodes_by_id.empty() || roots->ids.empty()) {
      continue;
    }
    std::vector<Frame> stack;
    for (int64_t raw_root : roots->ids) {
      WalkOneRoot(*graph, static_cast<uint32_t>(raw_root), per_row[r], stack);
    }
    total += per_row[r].size();
  }

  if (duckdb_list_vector_reserve(output, total) == DuckDBError) {
    duckdb_scalar_function_set_error(info,
                                     "critical_path_walk: reserve failed");
    return;
  }
  duckdb_list_vector_set_size(output, total);
  duckdb_vector sv = duckdb_list_vector_get_child(output);
  duckdb_vector c[7];
  int64_t* co[7];
  for (int k = 0; k < 7; ++k) {
    c[k] = duckdb_struct_vector_get_child(sv, static_cast<idx_t>(k));
    co[k] = static_cast<int64_t*>(duckdb_vector_get_data(c[k]));
  }
  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output));
  idx_t cursor = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    entries[r].offset = cursor;
    entries[r].length = per_row[r].size();
    for (const ResultRow& row : per_row[r]) {
      co[0][cursor] = row.root_id;
      co[1][cursor] = row.depth;
      co[2][cursor] = row.ts;
      co[3][cursor] = row.dur;
      co[4][cursor] = row.blocker_id;
      co[5][cursor] = row.blocker_utid;
      co[6][cursor] = row.parent_id;
      ++cursor;
    }
  }
}

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
    return base::ErrStatus("RegisterCriticalPath: agg '%s' failed", name);
  }
  return base::OkStatus();
}

}  // namespace

base::Status RegisterCriticalPath(duckdb_connection conn) {
  RETURN_IF_ERROR(
      RegisterAgg(conn, "__intrinsic_wakeup_graph_agg", 7, GraphUpdate,
                  GraphCombine, BufStateSize<WakeupGraph>, BufInit<WakeupGraph>,
                  BufFinalize<WakeupGraph>, BufDestroy<WakeupGraph>));
  RETURN_IF_ERROR(RegisterAgg(conn, "__intrinsic_cp_roots_agg", 1, RootsUpdate,
                              RootsCombine, BufStateSize<RootArray>,
                              BufInit<RootArray>, BufFinalize<RootArray>,
                              BufDestroy<RootArray>));

  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, "__intrinsic_critical_path_walk");
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_scalar_function_add_parameter(f, bigint);  // graph handle
  duckdb_scalar_function_add_parameter(f, bigint);  // roots handle
  duckdb_logical_type members[7] = {bigint, bigint, bigint, bigint,
                                    bigint, bigint, bigint};
  const char* names[7] = {"root_id",    "depth",        "ts",       "dur",
                          "blocker_id", "blocker_utid", "parent_id"};
  duckdb_logical_type struct_type =
      duckdb_create_struct_type(members, names, 7);
  duckdb_logical_type list_of_struct = duckdb_create_list_type(struct_type);
  duckdb_scalar_function_set_return_type(f, list_of_struct);
  duckdb_scalar_function_set_function(f, CombineWalk);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_logical_type(&list_of_struct);
  duckdb_destroy_logical_type(&struct_type);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus(
        "RegisterCriticalPath: __intrinsic_critical_path_walk failed");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
