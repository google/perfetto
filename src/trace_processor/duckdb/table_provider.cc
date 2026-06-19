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

#include "src/trace_processor/duckdb/table_provider.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

constexpr char kFunctionName[] = "__perfetto_df";

// Finds the id column index, mirroring DataframeModule's FindIdColumnIndex: the
// first column named "id" or "_auto_id".
std::optional<uint32_t> FindIdColumnIndex(
    const std::vector<std::string>& names) {
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (names[i] == "id" || names[i] == "_auto_id") {
      return i;
    }
  }
  return std::nullopt;
}

// Maps a dataframe StorageType to its DuckDB logical type per D1 section 2.1:
// ALL integer kinds widen to BIGINT for parity with SQLite's uniform 64-bit
// numeric output; Double -> DOUBLE; String -> VARCHAR.
duckdb_logical_type LogicalTypeFor(const dataframe::StorageType& type) {
  using dataframe::StorageType;
  switch (type.index()) {
    case StorageType::GetTypeIndex<dataframe::Id>():
    case StorageType::GetTypeIndex<dataframe::Uint32>():
    case StorageType::GetTypeIndex<dataframe::Int32>():
    case StorageType::GetTypeIndex<dataframe::Int64>():
      return duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    case StorageType::GetTypeIndex<dataframe::Double>():
      return duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    case StorageType::GetTypeIndex<dataframe::String>():
      return duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    default:
      PERFETTO_FATAL("Unimplemented storage type");
  }
}

// Writes a single cell into a DuckDB output vector at row `row`. Numeric cells
// widen to int64 into the BIGINT buffer, doubles into the DOUBLE buffer, strings
// are copied into the vector, nulls set validity-invalid. Matches the
// `dataframe::CellCallback` overload set so the cursor can drive it directly.
struct VectorWriter : dataframe::CellCallback {
  void OnCell(int64_t v) {
    static_cast<int64_t*>(duckdb_vector_get_data(vec))[row] = v;
  }
  void OnCell(uint32_t v) { OnCell(static_cast<int64_t>(v)); }
  void OnCell(int32_t v) { OnCell(static_cast<int64_t>(v)); }
  void OnCell(double v) {
    static_cast<double*>(duckdb_vector_get_data(vec))[row] = v;
  }
  void OnCell(NullTermStringView v) {
    duckdb_vector_assign_string_element_len(vec, row, v.data(), v.size());
  }
  void OnCell(std::nullptr_t) {
    duckdb_vector_ensure_validity_writable(vec);
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vec), row);
  }

  duckdb_vector vec = nullptr;
  idx_t row = 0;
};

// Bind data: which provider entry this scan targets and (in output order) the
// dataframe column index each result vector carries. Populated in bind; the
// projection (which subset DuckDB actually wants) is applied in init.
struct BindData {
  const DuckDbTableProvider::Entry* entry = nullptr;
};

void DestroyBindData(void* p) {
  delete static_cast<BindData*>(p);
}

// Per-scan state. Holds the full-scan query plan and the cursor (which borrows
// from the plan, so the plan must outlive it), plus the projection: for each
// output vector position, the dataframe column index it carries.
struct InitData {
  dataframe::Dataframe::QueryPlan plan;
  dataframe::Cursor<dataframe::ErrorValueFetcher> cursor;
  std::vector<uint32_t> projected_cols;  // df col index per output vector.
  bool executed = false;
};

void DestroyInitData(void* p) {
  delete static_cast<InitData*>(p);
}

void Bind(duckdb_bind_info info) {
  auto* provider = static_cast<DuckDbTableProvider*>(
      duckdb_bind_get_extra_info(info));
  if (!provider) {
    duckdb_bind_set_error(info, "__perfetto_df: missing provider");
    return;
  }
  if (duckdb_bind_get_parameter_count(info) != 1) {
    duckdb_bind_set_error(info, "__perfetto_df: expected one VARCHAR parameter");
    return;
  }
  duckdb_value name_val = duckdb_bind_get_parameter(info, 0);
  char* name_c = duckdb_get_varchar(name_val);
  std::string name = name_c ? name_c : "";
  duckdb_free(name_c);
  duckdb_destroy_value(&name_val);

  const DuckDbTableProvider::Entry* entry = provider->Resolve(name);
  if (!entry) {
    std::string err = "__perfetto_df: unknown table '" + name + "'";
    duckdb_bind_set_error(info, err.c_str());
    return;
  }

  // Declare the VISIBLE result schema from the cached spec, in column order
  // (the synthetic `_auto_id` column is hidden to match SQLite). Projection
  // pushdown means DuckDB may later ask for only a subset (init).
  const dataframe::DataframeSpec& spec = entry->spec;
  for (uint32_t df_col : entry->visible_to_df_col) {
    duckdb_logical_type t = LogicalTypeFor(spec.column_specs[df_col].type);
    duckdb_bind_add_result_column(info, spec.column_names[df_col].c_str(), t);
    duckdb_destroy_logical_type(&t);
  }

  auto* bind_data = new BindData();
  bind_data->entry = entry;
  duckdb_bind_set_bind_data(info, bind_data, DestroyBindData);
  duckdb_bind_set_cardinality(info, entry->df.row_count(), true);
}

void Init(duckdb_init_info info) {
  // Single-threaded scan: one cursor + non-thread-safe StringPool reads.
  duckdb_init_set_max_threads(info, 1);

  auto* bind_data =
      static_cast<BindData*>(duckdb_init_get_bind_data(info));
  const DuckDbTableProvider::Entry* entry = bind_data->entry;
  const dataframe::Dataframe& df = entry->df;

  auto init = std::make_unique<InitData>();

  // Read DuckDB's projection list (which result columns it actually wants, in
  // output order) and build a cols_used_bitmap with a bit set per projected
  // dataframe column. Empty filters/distinct/sort + no limit => plain ordered
  // full scan.
  idx_t n = duckdb_init_get_column_count(info);
  init->projected_cols.reserve(n);
  uint64_t cols_used_bitmap = 0;
  for (idx_t i = 0; i < n; ++i) {
    // DuckDB's projection index is into the VISIBLE (declared) result columns;
    // translate it to the underlying dataframe column index.
    auto visible_col =
        static_cast<uint32_t>(duckdb_init_get_column_index(info, i));
    PERFETTO_CHECK(visible_col < entry->visible_to_df_col.size());
    uint32_t df_col = entry->visible_to_df_col[visible_col];
    init->projected_cols.push_back(df_col);
    PERFETTO_CHECK(df_col < 64);
    cols_used_bitmap |= (uint64_t{1} << df_col);
  }

  std::vector<dataframe::FilterSpec> filter_specs;
  std::vector<dataframe::DistinctSpec> distinct_specs;
  std::vector<dataframe::SortSpec> sort_specs;
  dataframe::LimitSpec limit_spec;
  base::StatusOr<dataframe::Dataframe::QueryPlan> plan = df.PlanQuery(
      filter_specs, distinct_specs, sort_specs, limit_spec, cols_used_bitmap);
  if (!plan.ok()) {
    duckdb_init_set_error(info, plan.status().c_message());
    return;
  }
  init->plan = std::move(plan.value());
  df.PrepareCursor(init->plan, init->cursor);

  duckdb_init_set_init_data(info, init.release(), DestroyInitData);
}

void Main(duckdb_function_info info, duckdb_data_chunk output) {
  auto* init = static_cast<InitData*>(duckdb_function_get_init_data(info));

  if (!init->executed) {
    dataframe::ErrorValueFetcher fetcher;
    init->cursor.Execute(fetcher);
    init->executed = true;
  }

  const idx_t cap = duckdb_vector_size();
  const uint32_t num_out_cols =
      static_cast<uint32_t>(init->projected_cols.size());

  idx_t chunk_rows = 0;
  for (; chunk_rows < cap && !init->cursor.Eof(); ++chunk_rows) {
    for (uint32_t out_col = 0; out_col < num_out_cols; ++out_col) {
      VectorWriter writer;
      writer.vec = duckdb_data_chunk_get_vector(output, out_col);
      writer.row = chunk_rows;
      init->cursor.Cell(init->projected_cols[out_col], writer);
    }
    init->cursor.Next();
  }

  // size 0 signals EOF to DuckDB.
  duckdb_data_chunk_set_size(output, chunk_rows);
}

// Replacement scan: fired by DuckDB for a catalog name it can't resolve. On a
// provider hit, rewrite the reference to `__perfetto_df('<table_name>')`; on a
// miss leave the function name unset so DuckDB raises its normal catalog error.
void ReplacementScan(duckdb_replacement_scan_info info,
                     const char* table_name,
                     void* data) {
  auto* provider = static_cast<DuckDbTableProvider*>(data);
  if (!provider || !table_name) {
    return;
  }
  // Miss: leave unset. DuckDB then raises its normal "table does not exist"
  // error; the future routing/fallback layer keys on that.
  if (!provider->Resolve(table_name)) {
    return;
  }
  duckdb_replacement_scan_set_function_name(info, kFunctionName);
  duckdb_value name_val = duckdb_create_varchar(table_name);
  duckdb_replacement_scan_add_parameter(info, name_val);
  duckdb_destroy_value(&name_val);
}

}  // namespace

DuckDbTableProvider::DuckDbTableProvider(StringPool* string_pool,
                                         Resolver resolver)
    : string_pool_(string_pool), resolver_(std::move(resolver)) {}

DuckDbTableProvider::~DuckDbTableProvider() = default;

const DuckDbTableProvider::Entry* DuckDbTableProvider::InsertSnapshot(
    const std::string& name,
    const dataframe::Dataframe& df) {
  std::optional<uint32_t> id_col = FindIdColumnIndex(df.column_names());
  if (!id_col) {
    return nullptr;
  }
  auto entry = std::make_unique<Entry>(df.CopyFinalized());
  entry->id_col_idx = *id_col;
  // Build the visible-column mapping: every column except a synthetic
  // `_auto_id` (hidden to match SQLite's HIDDEN runtime-table id column, so
  // `SELECT *` excludes it). A real `id` column stays visible.
  const std::vector<std::string>& col_names = entry->spec.column_names;
  entry->visible_to_df_col.reserve(col_names.size());
  for (uint32_t i = 0; i < col_names.size(); ++i) {
    if (col_names[i] == "_auto_id") {
      continue;
    }
    entry->visible_to_df_col.push_back(i);
  }
  // Record the live source for read-through staleness detection.
  entry->source = &df;
  entry->source_mutations = df.mutations();
  // Replace any existing (possibly stale) snapshot under this name.
  entries_[name] = std::move(entry);
  return entries_[name].get();
}

base::Status DuckDbTableProvider::Register(const std::string& name,
                                           const dataframe::Dataframe& df) {
  if (entries_.count(name)) {
    return base::ErrStatus("DuckDbTableProvider: table '%s' already registered",
                           name.c_str());
  }
  if (!FindIdColumnIndex(df.column_names())) {
    return base::ErrStatus(
        "DuckDbTableProvider: table '%s' has no id/_auto_id column",
        name.c_str());
  }
  InsertSnapshot(name, df);
  return base::OkStatus();
}

const DuckDbTableProvider::Entry* DuckDbTableProvider::Find(
    const std::string& name) const {
  auto it = entries_.find(name);
  return it == entries_.end() ? nullptr : it->second.get();
}

const DuckDbTableProvider::Entry* DuckDbTableProvider::Resolve(
    const std::string& name) {
  const Entry* cached = Find(name);

  // No resolver: pure cache (used by eager `Register`-only callers, e.g. the
  // static-table tests). Cache entries never go stale because the caller owns
  // their lifetime.
  if (!resolver_) {
    return cached;
  }

  // Read-through: consult the live registry every time so a runtime table that
  // was dropped/recreated (or had an index added) - which swaps the backing
  // dataframe to a NEW object (verified) - is re-snapshotted instead of serving
  // a stale snapshot. The mutation count is a defensive secondary check for the
  // (currently unobserved) in-place-mutation case.
  const dataframe::Dataframe* live = resolver_(name);
  if (!live || !live->finalized()) {
    // The live table is gone, or is a not-yet-finalized dataframe that cannot
    // be snapshotted (`CopyFinalized()` CHECK-fails on an unfinalized
    // dataframe). Either way treat it as unavailable: drop any stale snapshot so
    // a future query errors cleanly rather than reading a freed dataframe, and
    // return nullptr so the reference is deemed ineligible (-> fallback).
    if (cached) {
      entries_.erase(name);
    }
    return nullptr;
  }
  if (cached && cached->source == live &&
      cached->source_mutations == live->mutations()) {
    return cached;  // Still fresh.
  }
  // Stale or first sight: (re)snapshot.
  return InsertSnapshot(name, *live);
}

base::Status DuckDbTableProvider::RegisterTableFunction(
    duckdb_connection connection) {
  duckdb_table_function fn = duckdb_create_table_function();
  duckdb_table_function_set_name(fn, kFunctionName);

  duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_table_function_add_parameter(fn, varchar);
  duckdb_destroy_logical_type(&varchar);

  duckdb_table_function_set_bind(fn, Bind);
  duckdb_table_function_set_init(fn, Init);
  duckdb_table_function_set_function(fn, Main);
  duckdb_table_function_supports_projection_pushdown(fn, true);
  // The provider is the extra-info: shared by every bind/scan; it is NOT owned
  // by DuckDB (no destroy callback) because the provider's lifetime is managed
  // by the caller (it must outlive the connection's database).
  duckdb_table_function_set_extra_info(fn, this, nullptr);

  duckdb_state s = duckdb_register_table_function(connection, fn);
  duckdb_destroy_table_function(&fn);
  if (s == DuckDBError) {
    return base::ErrStatus(
        "DuckDbTableProvider: duckdb_register_table_function failed (name "
        "'__perfetto_df' may already be registered)");
  }
  return base::OkStatus();
}

base::Status DuckDbTableProvider::RegisterReplacementScan(duckdb_database db) {
  // The provider is the extra-data, NOT owned by DuckDB (null delete callback):
  // its lifetime is caller-managed and must outlive `db`.
  duckdb_add_replacement_scan(db, ReplacementScan, this, nullptr);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
