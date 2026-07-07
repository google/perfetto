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

#include "src/trace_processor/duckdb/sched_table_function.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Logical column ordering of the `sched_df` result, matching
// `tables/sched_tables.py`. The bind callback adds the result columns in this
// order and the init/main callbacks project from it.
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

// Same per-cell reader as M3a: normalises any numeric storage type to int64 or
// resolves a string. M3b reads cells via the existing public `GetCell`
// cell-callback API (option (a) from the M3b design) rather than adding a raw
// FlexVector accessor to the dataframe; this keeps the dataframe public surface
// unchanged. The bulk-memcpy zero-copy variant would need a new accessor and is
// noted as a follow-up perf step, not required to prove DuckDB-owns-the-scan.
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

// Extra-info attached to the table function: the snapshot of the sched
// dataframe that DuckDB scans, plus the resolved dataframe column indices.
// Owned by DuckDB; freed via DestroyExtraInfo when the table function is
// destroyed.
struct ExtraInfo {
  explicit ExtraInfo(dataframe::Dataframe df) : sched(std::move(df)) {}

  dataframe::Dataframe sched;  // CopyFinalized() snapshot (shallow shared-ptr).
  // Index of each logical sched column within the dataframe, indexed by
  // SchedCol (kId has no dataframe column - it is synthesised).
  uint32_t df_col[kNumCols] = {};
  bool has_df_col[kNumCols] = {};
};

void DestroyExtraInfo(void* p) {
  delete static_cast<ExtraInfo*>(p);
}

// Bind data: read-only after bind. We don't need extra per-query state beyond
// the extra-info, but DuckDB requires bind data to exist to carry the result
// schema; we keep a back-pointer to the extra-info for the init/main phases.
struct BindData {
  const ExtraInfo* extra = nullptr;
};

void DestroyBindData(void* p) {
  delete static_cast<BindData*>(p);
}

// Per-scan init data: the projection (which logical columns DuckDB asked for,
// in output order) and a cursor of the next row to emit. max_threads is pinned
// to 1 so this single cursor is sufficient and StringPool reads stay
// single-threaded.
struct InitData {
  // For each output vector position, the logical SchedCol it carries.
  uint32_t projected[kNumCols] = {};
  uint32_t num_projected = 0;
  uint32_t next_row = 0;
};

void DestroyInitData(void* p) {
  delete static_cast<InitData*>(p);
}

duckdb_logical_type LogicalTypeFor(SchedCol c) {
  switch (c) {
    case kId:
    case kUtid:
    case kUcpu:
      return duckdb_create_logical_type(DUCKDB_TYPE_UINTEGER);
    case kTs:
    case kDur:
      return duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    case kEndState:
      return duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    case kPriority:
      return duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    case kNumCols:
      break;
  }
  PERFETTO_FATAL("unreachable");
}

const char* NameFor(SchedCol c) {
  switch (c) {
    case kId:
      return "id";
    case kTs:
      return "ts";
    case kDur:
      return "dur";
    case kUtid:
      return "utid";
    case kEndState:
      return "end_state";
    case kPriority:
      return "priority";
    case kUcpu:
      return "ucpu";
    case kNumCols:
      break;
  }
  PERFETTO_FATAL("unreachable");
}

void Bind(duckdb_bind_info info) {
  auto* extra = static_cast<ExtraInfo*>(duckdb_bind_get_extra_info(info));
  if (!extra) {
    duckdb_bind_set_error(info, "sched_df: missing extra info");
    return;
  }
  // Declare the full result schema, in logical column order. Projection
  // pushdown means DuckDB may later ask for only a subset (handled in init).
  for (uint32_t c = 0; c < kNumCols; ++c) {
    duckdb_logical_type t = LogicalTypeFor(static_cast<SchedCol>(c));
    duckdb_bind_add_result_column(info, NameFor(static_cast<SchedCol>(c)), t);
    duckdb_destroy_logical_type(&t);
  }
  auto* bind_data = new BindData();
  bind_data->extra = extra;
  duckdb_bind_set_bind_data(info, bind_data, DestroyBindData);
  duckdb_bind_set_cardinality(info, extra->sched.row_count(), true);
}

void Init(duckdb_init_info info) {
  // Single-threaded scan: one shared cursor in init data, StringPool reads stay
  // on one thread. (Parallelising would need per-thread cursors via local_init
  // and a thread-safe StringPool reader; out of scope for M3b.)
  duckdb_init_set_max_threads(info, 1);

  auto* init = new InitData();
  idx_t n = duckdb_init_get_column_count(info);
  PERFETTO_CHECK(n <= kNumCols);
  for (idx_t i = 0; i < n; ++i) {
    init->projected[i] =
        static_cast<uint32_t>(duckdb_init_get_column_index(info, i));
  }
  init->num_projected = static_cast<uint32_t>(n);
  init->next_row = 0;
  duckdb_init_set_init_data(info, init, DestroyInitData);
}

void Main(duckdb_function_info info, duckdb_data_chunk output) {
  auto* bind_data = static_cast<BindData*>(duckdb_function_get_bind_data(info));
  auto* init = static_cast<InitData*>(duckdb_function_get_init_data(info));
  const ExtraInfo* extra = bind_data->extra;
  const dataframe::Dataframe& df = extra->sched;

  const uint32_t row_count = df.row_count();
  const idx_t cap = duckdb_vector_size();

  idx_t chunk_rows = 0;
  if (init->next_row < row_count) {
    chunk_rows = row_count - init->next_row;
    if (chunk_rows > cap) {
      chunk_rows = cap;
    }
  }

  // size 0 signals EOF to DuckDB.
  if (chunk_rows == 0) {
    duckdb_data_chunk_set_size(output, 0);
    return;
  }

  const uint32_t base_row = init->next_row;
  for (uint32_t out_col = 0; out_col < init->num_projected; ++out_col) {
    auto logical = static_cast<SchedCol>(init->projected[out_col]);
    duckdb_vector vec = duckdb_data_chunk_get_vector(output, out_col);

    if (logical == kId) {
      // Synthesised id == row offset.
      auto* data = static_cast<uint32_t*>(duckdb_vector_get_data(vec));
      for (idx_t i = 0; i < chunk_rows; ++i) {
        data[i] = base_row + static_cast<uint32_t>(i);
      }
      continue;
    }

    PERFETTO_DCHECK(extra->has_df_col[logical]);
    const uint32_t df_col = extra->df_col[logical];

    if (logical == kEndState) {
      duckdb_vector_ensure_validity_writable(vec);
      uint64_t* validity = duckdb_vector_get_validity(vec);
      for (idx_t i = 0; i < chunk_rows; ++i) {
        CellReader cell;
        df.GetCell(base_row + static_cast<uint32_t>(i), df_col, cell);
        if (cell.is_null) {
          duckdb_validity_set_row_invalid(validity, i);
        } else {
          duckdb_vector_assign_string_element_len(
              vec, i, cell.string_value.data(), cell.string_value.size());
        }
      }
      continue;
    }

    // Numeric columns: write the converted int64 into the right-width buffer.
    switch (logical) {
      case kTs:
      case kDur: {
        auto* data = static_cast<int64_t*>(duckdb_vector_get_data(vec));
        for (idx_t i = 0; i < chunk_rows; ++i) {
          CellReader cell;
          df.GetCell(base_row + static_cast<uint32_t>(i), df_col, cell);
          data[i] = cell.int_value;
        }
        break;
      }
      case kUtid:
      case kUcpu: {
        auto* data = static_cast<uint32_t*>(duckdb_vector_get_data(vec));
        for (idx_t i = 0; i < chunk_rows; ++i) {
          CellReader cell;
          df.GetCell(base_row + static_cast<uint32_t>(i), df_col, cell);
          data[i] = static_cast<uint32_t>(cell.int_value);
        }
        break;
      }
      case kPriority: {
        auto* data = static_cast<int32_t*>(duckdb_vector_get_data(vec));
        for (idx_t i = 0; i < chunk_rows; ++i) {
          CellReader cell;
          df.GetCell(base_row + static_cast<uint32_t>(i), df_col, cell);
          data[i] = static_cast<int32_t>(cell.int_value);
        }
        break;
      }
      case kId:
      case kEndState:
      case kNumCols:
        PERFETTO_FATAL("unreachable");
    }
  }

  init->next_row = base_row + static_cast<uint32_t>(chunk_rows);
  duckdb_data_chunk_set_size(output, chunk_rows);
}

base::StatusOr<uint32_t> ColumnIndex(const dataframe::Dataframe& df,
                                     const char* name) {
  auto idx = df.IndexOfColumnLegacy(name);
  if (!idx) {
    return base::ErrStatus(
        "RegisterSchedTableFunction: dataframe is missing required sched "
        "column '%s'",
        name);
  }
  return *idx;
}

}  // namespace

base::Status RegisterSchedTableFunction(duckdb_connection connection,
                                        const dataframe::Dataframe& sched) {
  // Resolve the dataframe column indices up front so a schema mismatch fails
  // loudly at registration rather than mid-scan.
  struct ColSpec {
    SchedCol logical;
    const char* name;
  };
  static constexpr ColSpec kCols[] = {
      {kTs, "ts"},
      {kDur, "dur"},
      {kUtid, "utid"},
      {kEndState, "end_state"},
      {kPriority, "priority"},
      {kUcpu, "ucpu"},
  };

  auto extra = std::make_unique<ExtraInfo>(sched.CopyFinalized());
  for (const auto& spec : kCols) {
    base::StatusOr<uint32_t> idx = ColumnIndex(extra->sched, spec.name);
    if (!idx.ok()) {
      return idx.status();
    }
    extra->df_col[spec.logical] = *idx;
    extra->has_df_col[spec.logical] = true;
  }

  duckdb_table_function fn = duckdb_create_table_function();
  duckdb_table_function_set_name(fn, "sched_df");
  duckdb_table_function_set_bind(fn, Bind);
  duckdb_table_function_set_init(fn, Init);
  duckdb_table_function_set_function(fn, Main);
  duckdb_table_function_supports_projection_pushdown(fn, true);
  // Hand ownership of the snapshot to DuckDB; freed when the function is
  // destroyed (DB teardown). Released here from the unique_ptr only once the
  // set call has taken ownership.
  duckdb_table_function_set_extra_info(fn, extra.get(), DestroyExtraInfo);
  extra.release();

  duckdb_state s = duckdb_register_table_function(connection, fn);
  duckdb_destroy_table_function(&fn);
  if (s == DuckDBError) {
    return base::ErrStatus(
        "RegisterSchedTableFunction: duckdb_register_table_function failed "
        "(name 'sched_df' may already be registered)");
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
