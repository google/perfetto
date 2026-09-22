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

#include "src/trace_processor/perfetto_sql/engine/pipeline_module.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::Variant;

std::string CreateTableStmt(uint32_t column_count) {
  // Public names (which may repeat) are applied by the outer SELECT.
  std::vector<std::string> columns;
  for (uint32_t i = 0; i < column_count; ++i) {
    columns.push_back("c" + std::to_string(i));
  }
  columns.emplace_back("plan HIDDEN");
  return "CREATE TABLE x(" + base::Join(columns, ", ") + ")";
}

void ResultString(sqlite3_context* ctx, StringPool* pool, StringPool::Id id) {
  if (id.is_null()) {
    sqlite::result::Null(ctx);
    return;
  }
  // Interned strings live as long as the pool.
  auto str = pool->Get(id);
  sqlite::result::StaticString(ctx, str.c_str(), static_cast<int>(str.size()));
}

void ResultVariant(sqlite3_context* ctx,
                   StringPool* pool,
                   const ColumnView& view,
                   uint32_t row) {
  Variant cell = view.Value<Variant>(row);
  switch (cell.type) {
    case Variant::Type::kNull:
      return sqlite::result::Null(ctx);
    case Variant::Type::kInt64:
      return sqlite::result::Long(ctx, cell.AsInt64());
    case Variant::Type::kDouble:
      return sqlite::result::Double(ctx, cell.AsDouble());
    case Variant::Type::kString:
      return ResultString(ctx, pool, cell.AsString());
  }
  PERFETTO_FATAL("For GCC");
}

template <typename T, bool Nullable>
void ResultFlat(sqlite3_context* ctx,
                StringPool* pool,
                const ColumnView& view,
                uint32_t row) {
  uint32_t index = view.selection().GetIndex(row);
  if constexpr (Nullable) {
    if (!view.validity()->is_set(index)) {
      return sqlite::result::Null(ctx);
    }
  }
  T value = static_cast<const T*>(view.data())[index];
  if constexpr (std::is_same_v<T, double>) {
    sqlite::result::Double(ctx, value);
  } else if constexpr (std::is_same_v<T, StringPool::Id>) {
    ResultString(ctx, pool, value);
  } else {
    sqlite::result::Long(ctx, value);
  }
}

void ResultSequence(sqlite3_context* ctx,
                    StringPool*,
                    const ColumnView& view,
                    uint32_t row) {
  uint32_t index = view.selection().GetIndex(row);
  if (view.validity() && !view.validity()->is_set(index)) {
    return sqlite::result::Null(ctx);
  }
  sqlite::result::Long(ctx, index);
}

template <typename T>
PipelineModule::Cursor::ResultFn FlatReader(const ColumnView& view) {
  return view.validity() ? &ResultFlat<T, true> : &ResultFlat<T, false>;
}

PipelineModule::Cursor::ResultFn ReaderFor(const ColumnView& view) {
  if (view.kind() == ColumnView::Kind::kVariant) {
    return &ResultVariant;
  }
  if (view.kind() == ColumnView::Kind::kSequence) {
    return &ResultSequence;
  }
  core::StorageType type = view.type();
  if (type.Is<core::Uint32>())
    return FlatReader<uint32_t>(view);
  if (type.Is<core::Int32>())
    return FlatReader<int32_t>(view);
  if (type.Is<core::Int64>())
    return FlatReader<int64_t>(view);
  if (type.Is<core::Double>())
    return FlatReader<double>(view);
  if (type.Is<core::String>())
    return FlatReader<StringPool::Id>(view);
  PERFETTO_FATAL("Unexpected column type");
}

// Refreshes per-column readers only when entering a new batch.
void CacheColumnReaders(PipelineModule::Cursor* c) {
  c->columns.clear();
  for (const auto& column : c->plan->columns()) {
    const auto& view = c->rows->batch().column(column.index);
    c->columns.push_back({&view, ReaderFor(view)});
  }
}

// Surfaces the executor's error, if any, once rows stop.
int CheckStatus(PipelineModule::Cursor* cursor) {
  // status() walks every node so only check it once rows stop.
  base::Status status = cursor->rows->status();
  return status.ok() ? SQLITE_OK
                     : sqlite::utils::SetError(cursor->pVtab, status);
}

}  // namespace

PipelineModule::Invocation::~Invocation() {
  context->retired_tables.push_back(std::move(table));
}

base::Status PipelineModule::Context::Cleanup(sqlite3* db) {
  // Otherwise a rollback could resurrect a table after we removed its entry.
  if (!sqlite3_get_autocommit(db))
    return base::OkStatus();
  while (!retired_tables.empty()) {
    std::string sql = "DROP TABLE IF EXISTS temp." + retired_tables.back();
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    if (rc == SQLITE_LOCKED || rc == SQLITE_BUSY)
      return base::OkStatus();
    if (rc != SQLITE_OK)
      return base::ErrStatus("%s", sqlite3_errmsg(db));
    retired_tables.pop_back();
  }
  return base::OkStatus();
}

base::StatusOr<SqliteConnection::PreparedStatement> PipelineModule::Prepare(
    SqliteConnection* connection,
    Context* context,
    std::unique_ptr<pipeline::PhysicalPlan> plan,
    const SqlSource& source) {
  RETURN_IF_ERROR(context->Cleanup(connection->db()));
  std::string table_name =
      "__intrinsic_pipeline_" + std::to_string(context->next_table++);
  std::string table = "temp." + table_name;
  {
    auto create = connection->PrepareStatement(
        SqlSource::FromTraceProcessorImplementation(
            "CREATE VIRTUAL TABLE " + table + " USING " + kName + "(" +
            std::to_string(plan->columns().size()) + ")"));
    RETURN_IF_ERROR(create.status());
    create.Step();
    RETURN_IF_ERROR(create.status());
  }
  auto invocation = std::make_unique<Invocation>();
  invocation->context = context;
  invocation->table = std::move(table_name);
  invocation->plan = std::move(plan);
  std::vector<std::string> columns;
  for (size_t i = 0; i < invocation->plan->columns().size(); ++i) {
    std::string name =
        base::ReplaceAll(invocation->plan->columns()[i].name, "\"", "\"\"");
    columns.push_back("c" + std::to_string(i) + " AS \"" + name + "\"");
  }
  auto stmt = connection->PrepareStatement(source.RewriteAllIgnoreExisting(
      SqlSource::FromTraceProcessorImplementation(
          "SELECT " + base::Join(columns, ", ") + " FROM " + table + "(?)")));
  RETURN_IF_ERROR(stmt.status());
  // Finalization retires the table, so it can be dropped straight after. A
  // failure here leaves the table retired and is reported by the next Prepare.
  stmt.SetOnFinalized([context, db = connection->db()] {
    base::ignore_result(context->Cleanup(db));
  });
  // The hidden plan argument is consumed by xFilter. SQLite releases the plan
  // on finalization (also if binding fails), without a generic KeepAlive hook.
  int rc = sqlite3_bind_pointer(
      stmt.sqlite_stmt(), 1, invocation.release(), kPlanPointerType,
      [](void* p) { delete static_cast<Invocation*>(p); });
  if (rc != SQLITE_OK)
    return base::ErrStatus("%s", sqlite3_errmsg(connection->db()));
  return std::move(stmt);
}

int PipelineModule::Create(sqlite3* db,
                           void* raw_ctx,
                           int argc,
                           const char* const* argv,
                           sqlite3_vtab** vtab,
                           char** error) {
  if (argc != 4 || std::string(argv[1]) != "temp") {
    *error = sqlite3_mprintf("pipeline tables require TEMP and a column count");
    return SQLITE_ERROR;
  }
  auto count = base::CStringToUInt32(argv[3]);
  if (!count || *count == 0) {
    *error = sqlite3_mprintf("invalid pipeline column count");
    return SQLITE_ERROR;
  }
  std::string create_stmt = CreateTableStmt(*count);
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK)
    return r;
  auto res = std::make_unique<Vtab>();
  res->context = GetContext(raw_ctx);
  res->column_count = *count;
  *vtab = res.release();
  return SQLITE_OK;
}

int PipelineModule::Connect(sqlite3* db,
                            void* raw_ctx,
                            int argc,
                            const char* const* argv,
                            sqlite3_vtab** vtab,
                            char** error) {
  return Create(db, raw_ctx, argc, argv, vtab, error);
}

int PipelineModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  return SQLITE_OK;
}

int PipelineModule::Destroy(sqlite3_vtab* vtab) {
  return Disconnect(vtab);
}

int PipelineModule::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  int plan = -1;
  int rowid = -1;
  for (int i = 0; i < info->nConstraint; ++i) {
    const auto& constraint = info->aConstraint[i];
    if (!constraint.usable || constraint.op != SQLITE_INDEX_CONSTRAINT_EQ)
      continue;
    if (constraint.iColumn == static_cast<int>(GetVtab(tab)->column_count))
      plan = i;
    if (constraint.iColumn == -1)
      rowid = i;
  }
  if (plan == -1)
    return SQLITE_CONSTRAINT;
  info->aConstraintUsage[plan].argvIndex = 1;
  info->aConstraintUsage[plan].omit = true;
  if (rowid != -1) {
    info->aConstraintUsage[rowid].argvIndex = 2;
    // SQLite rechecks comparisons, including non-integer RHS values.
    info->idxNum = 1;
    info->estimatedRows = 1;
  }
  // Output rowid constraints run after the fold. General predicates remain
  // with SQLite; moving them below accumulation could change ancestor totals.
  info->estimatedCost = rowid == -1 ? 1e9 : 1e6;
  return SQLITE_OK;
}

int PipelineModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  *cursor = std::make_unique<Cursor>().release();
  return SQLITE_OK;
}

int PipelineModule::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int PipelineModule::Filter(sqlite3_vtab_cursor* cursor,
                           int idx_num,
                           const char*,
                           int argc,
                           sqlite3_value** argv) {
  Cursor* c = GetCursor(cursor);
  PERFETTO_DCHECK(argc == (idx_num ? 2 : 1));
  auto* invocation = static_cast<Invocation*>(
      sqlite3_value_pointer(argv[0], kPlanPointerType));
  Vtab* vtab = GetVtab(cursor->pVtab);
  if (!invocation || invocation->context != vtab->context ||
      invocation->plan->columns().size() != vtab->column_count) {
    return sqlite::utils::SetError(cursor->pVtab,
                                   "pipeline requires a matching bound plan");
  }
  c->plan = invocation->plan.get();
  c->pool = invocation->context->pool;
  c->rows = std::make_unique<core::exec::RowCursor>(c->plan->source());
  c->rowid = 0;
  c->target_rowid.reset();
  if (idx_num && sqlite3_value_type(argv[1]) == SQLITE_INTEGER) {
    c->target_rowid = sqlite3_value_int64(argv[1]);
    if (*c->target_rowid < 0) {
      c->eof = true;
      return SQLITE_OK;
    }
  }
  c->eof = !c->rows->Open();
  if (c->target_rowid) {
    while (!c->eof && c->rowid < *c->target_rowid) {
      ++c->rowid;
      c->eof = !c->rows->Next();
    }
  }
  if (c->eof)
    return CheckStatus(c);
  CacheColumnReaders(c);
  return SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
  if (c->target_rowid) {
    c->eof = true;
    return SQLITE_OK;
  }
  ++c->rowid;
  c->eof = !c->rows->Next();
  if (c->eof)
    return CheckStatus(c);
  if (c->rows->row() == 0)
    CacheColumnReaders(c);
  return SQLITE_OK;
}

int PipelineModule::Eof(sqlite3_vtab_cursor* cursor) {
  return GetCursor(cursor)->eof;
}

int PipelineModule::Column(sqlite3_vtab_cursor* cursor,
                           sqlite3_context* ctx,
                           int raw_n) {
  Cursor* c = GetCursor(cursor);
  // The hidden argument is a SQL NULL outside xFilter.
  if (static_cast<uint32_t>(raw_n) == c->columns.size()) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }
  const auto& column = c->columns[static_cast<uint32_t>(raw_n)];
  column.result(ctx, c->pool, *column.view, c->rows->row());
  return SQLITE_OK;
}

int PipelineModule::Rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid) {
  *rowid = GetCursor(cursor)->rowid;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
