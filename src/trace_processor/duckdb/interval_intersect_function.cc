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

#include "src/trace_processor/duckdb/interval_intersect_function.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Must match the SQLite-side interval_intersect plugin: at most 15 input tables
// per call, surfaced as id_0..id_14 in the output struct.
constexpr uint32_t kMaxTables = 15;

// One table's collected intervals. `intervals[k].id` is the row's INDEX into
// `real_ids` (so we can carry the user's full int64 id without truncating it to
// the uint32 Interval::id), and `real_ids[index]` is the user-visible id.
struct IntervalBuffer {
  std::vector<Interval> intervals;
  std::vector<int64_t> real_ids;
};

// Process-global registry mapping an opaque handle (returned by the aggregate)
// to a collected buffer. The combiner consumes (erases) the handles it reads.
// Guarded by a mutex: a single query is single-threaded here, but DuckDB may
// run the per-table aggregates on worker threads.
class BufferRegistry {
 public:
  static BufferRegistry& Get() {
    static BufferRegistry* instance = new BufferRegistry();
    return *instance;
  }

  int64_t Insert(std::unique_ptr<IntervalBuffer> buf) {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t handle = next_++;
    buffers_.emplace(handle, std::move(buf));
    return handle;
  }

  std::unique_ptr<IntervalBuffer> Take(int64_t handle) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
      return nullptr;
    }
    std::unique_ptr<IntervalBuffer> buf = std::move(it->second);
    buffers_.erase(it);
    return buf;
  }

 private:
  std::mutex mu_;
  int64_t next_ = 1;
  std::unordered_map<int64_t, std::unique_ptr<IntervalBuffer>> buffers_;
};

// === Aggregate: __intrinsic_ii_agg(id, ts, dur) -> BIGINT handle ============

// The aggregate state is a single pointer to a heap IntervalBuffer (or null).
using AggState = IntervalBuffer*;

idx_t AggStateSize(duckdb_function_info) {
  return sizeof(AggState);
}

void AggInit(duckdb_function_info, duckdb_aggregate_state state) {
  *reinterpret_cast<AggState*>(state) = nullptr;
}

void AggUpdate(duckdb_function_info,
               duckdb_data_chunk input,
               duckdb_aggregate_state* states) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector id_vec = duckdb_data_chunk_get_vector(input, 0);
  duckdb_vector ts_vec = duckdb_data_chunk_get_vector(input, 1);
  duckdb_vector dur_vec = duckdb_data_chunk_get_vector(input, 2);
  auto* id_data = static_cast<int64_t*>(duckdb_vector_get_data(id_vec));
  auto* ts_data = static_cast<int64_t*>(duckdb_vector_get_data(ts_vec));
  auto* dur_data = static_cast<int64_t*>(duckdb_vector_get_data(dur_vec));
  uint64_t* id_valid = duckdb_vector_get_validity(id_vec);
  uint64_t* ts_valid = duckdb_vector_get_validity(ts_vec);
  uint64_t* dur_valid = duckdb_vector_get_validity(dur_vec);
  for (idx_t row = 0; row < rows; ++row) {
    // A NULL ts/dur row cannot form an interval; skip it (matches the SQLite
    // path, which filters such rows out before aggregation).
    if ((ts_valid && !duckdb_validity_row_is_valid(ts_valid, row)) ||
        (dur_valid && !duckdb_validity_row_is_valid(dur_valid, row))) {
      continue;
    }
    AggState& slot = *reinterpret_cast<AggState*>(states[row]);
    if (!slot) {
      slot = new IntervalBuffer();
    }
    int64_t ts = ts_data[row];
    int64_t dur = dur_data[row];
    int64_t id = (id_valid && !duckdb_validity_row_is_valid(id_valid, row))
                     ? 0
                     : id_data[row];
    auto index = static_cast<uint32_t>(slot->real_ids.size());
    slot->real_ids.push_back(id);
    Interval iv;
    iv.start = static_cast<uint64_t>(ts);
    iv.end = static_cast<uint64_t>(ts + dur);
    iv.id = index;
    slot->intervals.push_back(iv);
  }
}

void AggCombine(duckdb_function_info,
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
      dst = new IntervalBuffer();
    }
    // Append src's rows, re-basing their index into dst->real_ids.
    auto base = static_cast<uint32_t>(dst->real_ids.size());
    dst->real_ids.insert(dst->real_ids.end(), src->real_ids.begin(),
                         src->real_ids.end());
    for (Interval iv : src->intervals) {
      iv.id += base;
      dst->intervals.push_back(iv);
    }
  }
}

void AggFinalize(duckdb_function_info,
                 duckdb_aggregate_state* source,
                 duckdb_vector result,
                 idx_t count,
                 idx_t offset) {
  auto* out = static_cast<int64_t*>(duckdb_vector_get_data(result));
  for (idx_t i = 0; i < count; ++i) {
    AggState& slot = *reinterpret_cast<AggState*>(source[i]);
    std::unique_ptr<IntervalBuffer> buf(slot ? slot : new IntervalBuffer());
    slot = nullptr;  // ownership transferred to the registry; destroy() no-ops.
    out[offset + i] = BufferRegistry::Get().Insert(std::move(buf));
  }
}

void AggDestroy(duckdb_aggregate_state* states, idx_t count) {
  for (idx_t i = 0; i < count; ++i) {
    AggState& slot = *reinterpret_cast<AggState*>(states[i]);
    delete slot;
    slot = nullptr;
  }
}

// === N-way intersection (lifted from the interval_intersect plugin) =========

struct MultiIndexInterval {
  uint64_t start;
  uint64_t end;
  std::array<uint32_t, kMaxTables> idx_in_table;
};

// Sorts a buffer's intervals by start and reports whether they are
// non-overlapping (so the intersector can use binary search safely).
bool SortAndCheckNonOverlapping(std::vector<Interval>& intervals) {
  std::sort(intervals.begin(), intervals.end(),
            [](const Interval& a, const Interval& b) {
              return a.start < b.start;
            });
  bool non_overlapping = true;
  for (size_t i = 1; i < intervals.size(); ++i) {
    if (intervals[i].start < intervals[i - 1].end) {
      non_overlapping = false;
      break;
    }
  }
  return non_overlapping;
}

// Runs the N-way intersection over the prepared per-table buffers. Mirrors
// PushPartition() in the interval_intersect plugin: seed from the smallest
// table, then iteratively clip against each remaining table via
// IntervalIntersector. Output rows carry, per table, the INDEX into that
// table's buffer (mapped back to the user id by the caller).
std::vector<MultiIndexInterval> IntersectNWay(
    const std::vector<IntervalBuffer*>& tables,
    const std::vector<bool>& non_overlapping) {
  size_t tables_count = tables.size();
  // An empty input table makes the whole intersection empty.
  for (IntervalBuffer* t : tables) {
    if (t->intervals.empty()) {
      return {};
    }
  }

  std::vector<uint32_t> order(tables_count);
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return tables[a]->intervals.size() < tables[b]->intervals.size();
  });

  uint32_t smallest = order.front();
  std::vector<MultiIndexInterval> last;
  last.reserve(tables[smallest]->intervals.size());
  for (const Interval& iv : tables[smallest]->intervals) {
    MultiIndexInterval m{};
    m.start = iv.start;
    m.end = iv.end;
    m.idx_in_table[smallest] = iv.id;
    last.push_back(m);
  }

  std::vector<MultiIndexInterval> next;
  for (uint32_t i = 1; i < tables_count && !last.empty(); ++i) {
    next.clear();
    uint32_t ti = order[i];
    IntervalIntersector::Mode mode = IntervalIntersector::DecideMode(
        non_overlapping[ti], static_cast<uint32_t>(last.size()));
    IntervalIntersector intersector(tables[ti]->intervals, mode);
    for (const MultiIndexInterval& prev : last) {
      std::vector<Interval> overlaps;
      intersector.FindOverlaps(prev.start, prev.end, overlaps);
      for (const Interval& ov : overlaps) {
        MultiIndexInterval m = prev;
        m.idx_in_table[ti] = ov.id;
        m.start = ov.start;
        m.end = ov.end;
        next.push_back(m);
      }
    }
    last = std::move(next);
  }
  return last;
}

// === Scalar combiner: __intrinsic_ii_combine(LIST<BIGINT>) ->
//     LIST<STRUCT(ts, dur, id_0..id_14)> ====================================

void Combine(duckdb_function_info info,
             duckdb_data_chunk input,
             duckdb_vector output) {
  idx_t out_rows = duckdb_data_chunk_get_size(input);
  duckdb_vector list_vec = duckdb_data_chunk_get_vector(input, 0);
  auto* list_entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(list_vec));
  duckdb_vector handle_vec = duckdb_list_vector_get_child(list_vec);
  auto* handle_data = static_cast<int64_t*>(duckdb_vector_get_data(handle_vec));
  uint64_t* handle_valid = duckdb_vector_get_validity(handle_vec);

  // Compute every output row's result first so we know the total child count to
  // reserve in the result LIST vector.
  std::vector<std::vector<MultiIndexInterval>> per_row(out_rows);
  std::vector<size_t> per_row_tables(out_rows, 0);
  // Keep the taken buffers alive until we have mapped indices -> user ids.
  std::vector<std::vector<std::unique_ptr<IntervalBuffer>>> per_row_bufs(
      out_rows);
  idx_t total = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    duckdb_list_entry entry = list_entries[r];
    std::vector<IntervalBuffer*> tables;
    auto& owned = per_row_bufs[r];
    bool ok = true;
    for (idx_t k = 0; k < entry.length && k < kMaxTables; ++k) {
      idx_t pos = entry.offset + k;
      if (handle_valid && !duckdb_validity_row_is_valid(handle_valid, pos)) {
        ok = false;
        break;
      }
      std::unique_ptr<IntervalBuffer> buf =
          BufferRegistry::Get().Take(handle_data[pos]);
      if (!buf) {
        ok = false;
        break;
      }
      tables.push_back(buf.get());
      owned.push_back(std::move(buf));
    }
    if (!ok || tables.empty()) {
      continue;
    }
    std::vector<bool> non_overlapping(tables.size());
    for (size_t t = 0; t < tables.size(); ++t) {
      non_overlapping[t] = SortAndCheckNonOverlapping(tables[t]->intervals);
    }
    per_row_tables[r] = tables.size();
    per_row[r] = IntersectNWay(tables, non_overlapping);
    total += per_row[r].size();
  }

  if (duckdb_list_vector_reserve(output, total) == DuckDBError) {
    duckdb_scalar_function_set_error(info, "ii_combine: list reserve failed");
    return;
  }
  duckdb_list_vector_set_size(output, total);
  duckdb_vector struct_vec = duckdb_list_vector_get_child(output);

  // Struct children: 0 = ts, 1 = dur, 2..(2+kMaxTables-1) = id_0..id_14.
  duckdb_vector ts_child = duckdb_struct_vector_get_child(struct_vec, 0);
  duckdb_vector dur_child = duckdb_struct_vector_get_child(struct_vec, 1);
  auto* ts_out = static_cast<int64_t*>(duckdb_vector_get_data(ts_child));
  auto* dur_out = static_cast<int64_t*>(duckdb_vector_get_data(dur_child));
  std::array<duckdb_vector, kMaxTables> id_child{};
  std::array<int64_t*, kMaxTables> id_out{};
  for (uint32_t j = 0; j < kMaxTables; ++j) {
    id_child[j] = duckdb_struct_vector_get_child(struct_vec, 2 + j);
    id_out[j] = static_cast<int64_t*>(duckdb_vector_get_data(id_child[j]));
    duckdb_vector_ensure_validity_writable(id_child[j]);
  }

  auto* out_entries =
      static_cast<duckdb_list_entry*>(duckdb_vector_get_data(output));
  idx_t cursor = 0;
  for (idx_t r = 0; r < out_rows; ++r) {
    out_entries[r].offset = cursor;
    out_entries[r].length = per_row[r].size();
    size_t n_tables = per_row_tables[r];
    for (const MultiIndexInterval& m : per_row[r]) {
      ts_out[cursor] = static_cast<int64_t>(m.start);
      dur_out[cursor] =
          static_cast<int64_t>(m.end) - static_cast<int64_t>(m.start);
      for (uint32_t j = 0; j < kMaxTables; ++j) {
        uint64_t* validity = duckdb_vector_get_validity(id_child[j]);
        if (j < n_tables) {
          id_out[j][cursor] =
              per_row_bufs[r][j]->real_ids[m.idx_in_table[j]];
          duckdb_validity_set_row_valid(validity, cursor);
        } else {
          duckdb_validity_set_row_invalid(validity, cursor);
        }
      }
      ++cursor;
    }
  }
}

base::Status RegisterAgg(duckdb_connection conn) {
  duckdb_aggregate_function f = duckdb_create_aggregate_function();
  duckdb_aggregate_function_set_name(f, "__intrinsic_ii_agg");
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_aggregate_function_add_parameter(f, bigint);  // id
  duckdb_aggregate_function_add_parameter(f, bigint);  // ts
  duckdb_aggregate_function_add_parameter(f, bigint);  // dur
  duckdb_aggregate_function_set_return_type(f, bigint);
  duckdb_destroy_logical_type(&bigint);
  duckdb_aggregate_function_set_functions(f, AggStateSize, AggInit, AggUpdate,
                                          AggCombine, AggFinalize);
  duckdb_aggregate_function_set_destructor(f, AggDestroy);
  duckdb_state st = duckdb_register_aggregate_function(conn, f);
  duckdb_destroy_aggregate_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterIntervalIntersect: agg registration failed");
  }
  return base::OkStatus();
}

base::Status RegisterCombine(duckdb_connection conn) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, "__intrinsic_ii_combine");

  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type list_of_bigint = duckdb_create_list_type(bigint);
  duckdb_scalar_function_add_parameter(f, list_of_bigint);

  // Return type: LIST<STRUCT(ts, dur, id_0 .. id_14)>, all BIGINT.
  std::array<duckdb_logical_type, 2 + kMaxTables> members{};
  std::array<const char*, 2 + kMaxTables> names{};
  std::array<std::string, kMaxTables> id_names;
  members[0] = bigint;
  names[0] = "ts";
  members[1] = bigint;
  names[1] = "dur";
  for (uint32_t j = 0; j < kMaxTables; ++j) {
    id_names[j] = "id_" + std::to_string(j);
    members[2 + j] = bigint;
    names[2 + j] = id_names[j].c_str();
  }
  duckdb_logical_type struct_type = duckdb_create_struct_type(
      members.data(), names.data(), members.size());
  duckdb_logical_type list_of_struct = duckdb_create_list_type(struct_type);
  duckdb_scalar_function_set_return_type(f, list_of_struct);

  duckdb_scalar_function_set_function(f, Combine);
  duckdb_state st = duckdb_register_scalar_function(conn, f);

  duckdb_destroy_logical_type(&list_of_struct);
  duckdb_destroy_logical_type(&struct_type);
  duckdb_destroy_logical_type(&list_of_bigint);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus(
        "RegisterIntervalIntersect: combine registration failed");
  }
  return base::OkStatus();
}

}  // namespace

base::Status RegisterIntervalIntersect(duckdb_connection conn) {
  RETURN_IF_ERROR(RegisterAgg(conn));
  RETURN_IF_ERROR(RegisterCombine(conn));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
