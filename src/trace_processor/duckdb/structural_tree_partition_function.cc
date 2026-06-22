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

#include "src/trace_processor/duckdb/structural_tree_partition_function.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Verbatim port of the structural_tree_partition plugin's Step/Final (the
// algorithm is plain index arithmetic over the tree). Collects (id, parent_id,
// group) rows; at finalize, counting-sorts by parent to build a children map,
// then a two-pass DFS associates each node with its nearest same-group ancestor.
constexpr uint32_t kNullParentId = std::numeric_limits<uint32_t>::max();

struct Row {
  uint32_t id;
  uint32_t parent_id;
  uint32_t group;
};

// Accumulated input (one per group/aggregate state).
struct Buffer {
  std::vector<Row> input;
  std::vector<uint32_t> child_count_by_id;
  std::optional<Row> root;
  uint32_t max_group = 0;
  bool multiple_roots = false;

  void EnsureSize(uint32_t id) {
    if (id >= child_count_by_id.size()) {
      child_count_by_id.resize(id + 1);
    }
  }
};

struct ResultRow {
  int64_t node_id;
  int64_t parent_node_id;
  bool has_parent;
  int64_t group_key;
};

std::vector<ResultRow> RunPartition(Buffer& buf) {
  std::vector<ResultRow> out;
  if (buf.multiple_roots || !buf.root || buf.input.empty()) {
    // The plugin errors on multiple/zero roots; the DuckDB lane returns empty
    // (the query then falls back, which errors honestly if SQLite also does).
    return out;
  }
  // Partial sums -> output positions; counting sort by parent_id.
  std::partial_sum(buf.child_count_by_id.cbegin(), buf.child_count_by_id.cend(),
                   buf.child_count_by_id.begin());
  std::vector<Row> sorted(buf.input.size());
  for (auto it = buf.input.rbegin(); it != buf.input.rend(); ++it) {
    PERFETTO_DCHECK(buf.child_count_by_id[it->parent_id] > 0);
    uint32_t index = --buf.child_count_by_id[it->parent_id];
    sorted[index] = *it;
  }
  // child_count_by_id is now the start offset of each id's children in `sorted`.
  auto children_begin = [&](uint32_t id) {
    return sorted.data() + buf.child_count_by_id[id];
  };
  auto children_end = [&](uint32_t id) {
    return id + 1 == buf.child_count_by_id.size()
               ? sorted.data() + sorted.size()
               : sorted.data() + buf.child_count_by_id[id + 1];
  };

  struct StackState {
    Row row;
    std::optional<uint32_t> prev_ancestor_id_for_group;
    bool first_pass_done;
  };
  std::vector<StackState> stack{{*buf.root, std::nullopt, false}};
  std::vector<std::optional<uint32_t>> ancestor_id_for_group(buf.max_group + 1,
                                                             std::nullopt);
  while (!stack.empty()) {
    StackState& ss = stack.back();
    if (ss.first_pass_done) {
      ancestor_id_for_group[ss.row.group] = ss.prev_ancestor_id_for_group;
      stack.pop_back();
      continue;
    }
    std::optional<uint32_t> anc = ancestor_id_for_group[ss.row.group];
    out.push_back(ResultRow{ss.row.id, anc ? *anc : 0, anc.has_value(),
                            ss.row.group});
    ss.first_pass_done = true;
    ss.prev_ancestor_id_for_group = anc;
    ancestor_id_for_group[ss.row.group] = ss.row.id;
    const Row* start = children_begin(ss.row.id);
    const Row* end = children_end(ss.row.id);
    for (const Row* it = start; it != end; ++it) {
      stack.push_back(StackState{*it, std::nullopt, false});
    }
  }
  return out;
}

using AggState = Buffer*;

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
  duckdb_vector id_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector pid_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector grp_vec = duckdb_data_chunk_get_vector(input, 2);
  auto* id_data = static_cast<int64_t*>(duckdb_vector_get_data(id_vec));
  auto* pid_data = static_cast<int64_t*>(duckdb_vector_get_data(pid_vec));
  auto* grp_data = static_cast<int64_t*>(duckdb_vector_get_data(grp_vec));
  uint64_t* id_valid = duckdb_vector_get_validity(id_vec);
  uint64_t* pid_valid = duckdb_vector_get_validity(pid_vec);
  for (idx_t row = 0; row < rows; ++row) {
    if (id_valid && !duckdb_validity_row_is_valid(id_valid, row)) {
      continue;  // A NULL id cannot be a tree node.
    }
    AggState& slot = *reinterpret_cast<AggState*>(states[row]);
    if (!slot) {
      slot = new Buffer();
    }
    Buffer& buf = *slot;
    auto id = static_cast<uint32_t>(id_data[row]);
    auto group = static_cast<uint32_t>(grp_data[row]);
    buf.max_group = std::max(buf.max_group, group);
    if (pid_valid && !duckdb_validity_row_is_valid(pid_valid, row)) {
      if (buf.root) {
        buf.multiple_roots = true;
      }
      buf.root = Row{id, kNullParentId, group};
      buf.EnsureSize(id);
      continue;
    }
    auto parent_id = static_cast<uint32_t>(pid_data[row]);
    buf.EnsureSize(std::max(id, parent_id));
    buf.child_count_by_id[parent_id]++;
    buf.input.push_back(Row{id, parent_id, group});
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
      dst = new Buffer();
    }
    Buffer& d = *dst;
    Buffer& s = *src;
    d.max_group = std::max(d.max_group, s.max_group);
    if (s.root) {
      if (d.root) {
        d.multiple_roots = true;
      }
      d.root = s.root;
    }
    d.multiple_roots = d.multiple_roots || s.multiple_roots;
    for (const Row& r : s.input) {
      d.EnsureSize(std::max(r.id, r.parent_id));
      d.child_count_by_id[r.parent_id]++;
      d.input.push_back(r);
    }
    if (s.root) {
      d.EnsureSize(s.root->id);
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
    std::unique_ptr<Buffer> buf(slot ? slot : new Buffer());
    slot = nullptr;
    per_group[i] = RunPartition(*buf);
    total += per_group[i].size();
  }

  duckdb_list_vector_reserve(result, total);
  duckdb_list_vector_set_size(result, total);
  duckdb_vector sv = duckdb_list_vector_get_child(result);
  duckdb_vector c_node = duckdb_struct_vector_get_child(sv, 0);
  duckdb_vector c_parent = duckdb_struct_vector_get_child(sv, 1);
  duckdb_vector c_group = duckdb_struct_vector_get_child(sv, 2);
  auto* node_out = static_cast<int64_t*>(duckdb_vector_get_data(c_node));
  auto* parent_out = static_cast<int64_t*>(duckdb_vector_get_data(c_parent));
  auto* group_out = static_cast<int64_t*>(duckdb_vector_get_data(c_group));
  duckdb_vector_ensure_validity_writable(c_parent);
  uint64_t* parent_valid = duckdb_vector_get_validity(c_parent);

  auto* entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(result));
  idx_t cursor = 0;
  for (idx_t i = 0; i < count; ++i) {
    entries[offset + i].offset = cursor;
    entries[offset + i].length = per_group[i].size();
    for (const ResultRow& r : per_group[i]) {
      node_out[cursor] = r.node_id;
      group_out[cursor] = r.group_key;
      if (r.has_parent) {
        parent_out[cursor] = r.parent_node_id;
        duckdb_validity_set_row_valid(parent_valid, cursor);
      } else {
        duckdb_validity_set_row_invalid(parent_valid, cursor);
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

base::Status RegisterStructuralTreePartition(duckdb_connection conn) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, "__intrinsic_structural_tree_partition");
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_aggregate_function_add_parameter(f, bigint);  // id
  duckdb_aggregate_function_add_parameter(f, bigint);  // parent_id
  duckdb_aggregate_function_add_parameter(f, bigint);  // group_key

  duckdb_logical_type members[3] = {bigint, bigint, bigint};
  const char* names[3] = {"node_id", "parent_node_id", "group_key"};
  duckdb_logical_type struct_type =
      duckdb_create_struct_type(members, names, 3);
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
    return base::ErrStatus(
        "RegisterStructuralTreePartition: registration failed");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
