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

#include "src/trace_processor/duckdb/sched_appender.h"

#include <cstdint>
#include <string>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Column ordering of the DuckDB `sched` table built by this file. Must match
// the CREATE TABLE statement below and the dataframe column lookups.
enum SchedCol : uint32_t {
  kId = 0,
  kTs = 1,
  kDur = 2,
  kUtid = 3,
  kEndState = 4,
  kPriority = 5,
  kUcpu = 6,
  kNumCols = 7,
};

// Reads a single cell out of the dataframe via the public runtime-dispatch
// callback API and normalises it into either an int64 (for any of the numeric
// storage types: Id/Uint32/Int32/Int64) or a string. M3a is correctness-first
// and explicitly NOT perf-representative, so per-cell reads are acceptable; the
// bulk memcpy of the contiguous FlexVector windows is part of the M3b zero-copy
// table function (which also needs raw buffer access the public Dataframe API
// does not currently expose).
struct CellReader : dataframe::CellCallback {
  void OnCell(int64_t v) {
    int_value = v;
    is_null = false;
  }
  void OnCell(uint32_t v) {
    int_value = static_cast<int64_t>(v);
    is_null = false;
  }
  void OnCell(int32_t v) {
    int_value = static_cast<int64_t>(v);
    is_null = false;
  }
  void OnCell(double v) {
    int_value = static_cast<int64_t>(v);
    is_null = false;
  }
  void OnCell(NullTermStringView v) {
    string_value.assign(v.data(), v.size());
    is_null = false;
  }
  void OnCell(std::nullptr_t) { is_null = true; }

  int64_t int_value = 0;
  std::string string_value;
  bool is_null = false;
};

// Resolves the index of a column in the dataframe by name, returning an error
// status if the column is missing (so a schema mismatch fails loudly rather
// than reading garbage).
base::StatusOr<uint32_t> ColumnIndex(const dataframe::Dataframe& df,
                                     const char* name) {
  auto idx = df.IndexOfColumnLegacy(name);
  if (!idx) {
    return base::ErrStatus(
        "AppendSchedDataframe: dataframe is missing required sched column '%s'",
        name);
  }
  return *idx;
}

base::Status MakeError(const char* what, const char* detail) {
  return base::ErrStatus("AppendSchedDataframe: %s: %s", what,
                         detail ? detail : "(no detail)");
}

}  // namespace

base::Status AppendSchedDataframe(duckdb_connection connection,
                                  const dataframe::Dataframe& sched) {
  // Resolve the dataframe column indices up front so we fail loudly on a schema
  // mismatch.
  base::StatusOr<uint32_t> ts_idx = ColumnIndex(sched, "ts");
  base::StatusOr<uint32_t> dur_idx = ColumnIndex(sched, "dur");
  base::StatusOr<uint32_t> utid_idx = ColumnIndex(sched, "utid");
  base::StatusOr<uint32_t> end_state_idx = ColumnIndex(sched, "end_state");
  base::StatusOr<uint32_t> priority_idx = ColumnIndex(sched, "priority");
  base::StatusOr<uint32_t> ucpu_idx = ColumnIndex(sched, "ucpu");
  for (const auto* s : {&ts_idx, &dur_idx, &utid_idx, &end_state_idx,
                        &priority_idx, &ucpu_idx}) {
    if (!s->ok()) {
      return s->status();
    }
  }

  // 1. Create the destination table.
  duckdb_result create_res;
  if (duckdb_query(connection,
                   "CREATE TABLE sched(id UINTEGER, ts BIGINT, dur BIGINT, "
                   "utid UINTEGER, end_state VARCHAR, priority INTEGER, "
                   "ucpu UINTEGER)",
                   &create_res) == DuckDBError) {
    base::Status s =
        MakeError("CREATE TABLE failed", duckdb_result_error(&create_res));
    duckdb_destroy_result(&create_res);
    return s;
  }
  duckdb_destroy_result(&create_res);

  // 2. Create the appender.
  duckdb_appender appender = nullptr;
  if (duckdb_appender_create(connection, nullptr, "sched", &appender) ==
      DuckDBError) {
    base::Status s =
        MakeError("appender_create failed", duckdb_appender_error(appender));
    duckdb_appender_destroy(&appender);
    return s;
  }

  // Logical types for the data chunk, in table-column order.
  duckdb_type col_types[kNumCols] = {
      DUCKDB_TYPE_UINTEGER,  // id
      DUCKDB_TYPE_BIGINT,    // ts
      DUCKDB_TYPE_BIGINT,    // dur
      DUCKDB_TYPE_UINTEGER,  // utid
      DUCKDB_TYPE_VARCHAR,   // end_state
      DUCKDB_TYPE_INTEGER,   // priority
      DUCKDB_TYPE_UINTEGER,  // ucpu
  };
  duckdb_logical_type logical_types[kNumCols];
  for (uint32_t c = 0; c < kNumCols; ++c) {
    logical_types[c] = duckdb_create_logical_type(col_types[c]);
  }

  const uint32_t row_count = sched.row_count();
  const idx_t chunk_capacity = duckdb_vector_size();

  base::Status status = base::OkStatus();
  uint32_t row = 0;
  while (row < row_count && status.ok()) {
    idx_t chunk_rows = chunk_capacity;
    if (row_count - row < chunk_rows) {
      chunk_rows = row_count - row;
    }

    duckdb_data_chunk chunk = duckdb_create_data_chunk(logical_types, kNumCols);

    // Grab the per-column vector data pointers once for this chunk.
    auto* id_data = static_cast<uint32_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kId)));
    auto* ts_data = static_cast<int64_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kTs)));
    auto* dur_data = static_cast<int64_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kDur)));
    auto* utid_data = static_cast<uint32_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kUtid)));
    duckdb_vector end_state_vec =
        duckdb_data_chunk_get_vector(chunk, kEndState);
    auto* priority_data = static_cast<int32_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kPriority)));
    auto* ucpu_data = static_cast<uint32_t*>(
        duckdb_vector_get_data(duckdb_data_chunk_get_vector(chunk, kUcpu)));

    // Make the end_state validity mask writable so we can mark nulls. The mask
    // starts all-valid; we clear bits only for null cells.
    duckdb_vector_ensure_validity_writable(end_state_vec);
    uint64_t* end_state_validity = duckdb_vector_get_validity(end_state_vec);

    for (idx_t i = 0; i < chunk_rows; ++i) {
      uint32_t src_row = row + static_cast<uint32_t>(i);

      // Synthesised id == row offset.
      id_data[i] = src_row;

      CellReader cell;
      sched.GetCell(src_row, *ts_idx, cell);
      ts_data[i] = cell.int_value;

      cell = CellReader{};
      sched.GetCell(src_row, *dur_idx, cell);
      dur_data[i] = cell.int_value;

      cell = CellReader{};
      sched.GetCell(src_row, *utid_idx, cell);
      utid_data[i] = static_cast<uint32_t>(cell.int_value);

      cell = CellReader{};
      sched.GetCell(src_row, *priority_idx, cell);
      priority_data[i] = static_cast<int32_t>(cell.int_value);

      cell = CellReader{};
      sched.GetCell(src_row, *ucpu_idx, cell);
      ucpu_data[i] = static_cast<uint32_t>(cell.int_value);

      cell = CellReader{};
      sched.GetCell(src_row, *end_state_idx, cell);
      if (cell.is_null) {
        duckdb_validity_set_row_invalid(end_state_validity, i);
      } else {
        duckdb_vector_assign_string_element_len(end_state_vec, i,
                                                cell.string_value.data(),
                                                cell.string_value.size());
      }
    }

    duckdb_data_chunk_set_size(chunk, chunk_rows);
    if (duckdb_append_data_chunk(appender, chunk) == DuckDBError) {
      status = MakeError("append_data_chunk failed",
                         duckdb_appender_error(appender));
    }
    duckdb_destroy_data_chunk(&chunk);
    row += static_cast<uint32_t>(chunk_rows);
  }

  if (status.ok() && duckdb_appender_flush(appender) == DuckDBError) {
    status =
        MakeError("appender_flush failed", duckdb_appender_error(appender));
  }

  duckdb_appender_destroy(&appender);
  for (uint32_t c = 0; c < kNumCols; ++c) {
    duckdb_destroy_logical_type(&logical_types[c]);
  }
  return status;
}

}  // namespace perfetto::trace_processor::duckdb_integration
