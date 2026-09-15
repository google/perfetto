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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_plan.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::Variant;

std::string CreateTableStmt(const pipeline::PipelinePlan& plan) {
  std::string stmt = "CREATE TABLE x(";
  bool first = true;
  for (const pipeline::PipelinePlan::Column& column : plan.columns()) {
    if (!first) {
      stmt += ", ";
    }
    first = false;
    stmt += '"';
    for (char c : column.name) {
      stmt += c;
      if (c == '"') {
        stmt += '"';
      }
    }
    stmt += '"';
  }
  stmt += ")";
  return stmt;
}

void ResultString(sqlite3_context* ctx, StringPool* pool, StringPool::Id id) {
  if (id.is_null()) {
    sqlite::result::Null(ctx);
    return;
  }
  // Interned strings stay put for as long as the pool does.
  sqlite::result::StaticString(ctx, pool->Get(id).c_str());
}

void ResultCell(sqlite3_context* ctx,
                StringPool* pool,
                const ColumnView& view,
                uint32_t row) {
  if (view.kind() == ColumnView::Kind::kVariant) {
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
  const core::BitVector* validity = view.validity();
  if (validity && !validity->is_set(view.selection().GetIndex(row))) {
    return sqlite::result::Null(ctx);
  }
  core::StorageType type = view.type();
  if (type.Is<core::Id>() || type.Is<core::Uint32>()) {
    return sqlite::result::Long(ctx, view.Value<uint32_t>(row));
  }
  if (type.Is<core::Int32>()) {
    return sqlite::result::Long(ctx, view.Value<int32_t>(row));
  }
  if (type.Is<core::Int64>()) {
    return sqlite::result::Long(ctx, view.Value<int64_t>(row));
  }
  if (type.Is<core::Double>()) {
    return sqlite::result::Double(ctx, view.Value<double>(row));
  }
  if (type.Is<core::String>()) {
    return ResultString(ctx, pool, view.Value<StringPool::Id>(row));
  }
  PERFETTO_FATAL("Unexpected column type");
}

}  // namespace

int PipelineModule::Connect(sqlite3* db,
                            void* raw_ctx,
                            int,
                            const char* const*,
                            sqlite3_vtab** vtab,
                            char**) {
  const Context* ctx = GetContext(raw_ctx);
  std::string create_stmt = CreateTableStmt(*ctx->plan);
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK) {
    return r;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->context = ctx;
  *vtab = res.release();
  return SQLITE_OK;
}

int PipelineModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  return SQLITE_OK;
}

int PipelineModule::BestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
  // Nothing is pushed into the pipeline: SQLite applies every constraint.
  info->estimatedCost = 1e9;
  return SQLITE_OK;
}

int PipelineModule::Open(sqlite3_vtab* tab, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  c->rows = std::make_unique<core::exec::RowCursor>(
      GetVtab(tab)->context->plan->source());
  *cursor = c.release();
  return SQLITE_OK;
}

int PipelineModule::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int PipelineModule::Filter(sqlite3_vtab_cursor* cursor,
                           int,
                           const char*,
                           int,
                           sqlite3_value**) {
  Cursor* c = GetCursor(cursor);
  c->rowid = 0;
  c->eof = !c->rows->Open();
  if (base::Status status = c->rows->status(); !status.ok()) {
    return sqlite::utils::SetError(cursor->pVtab, status);
  }
  return SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
  ++c->rowid;
  c->eof = !c->rows->Next();
  if (base::Status status = c->rows->status(); !status.ok()) {
    return sqlite::utils::SetError(cursor->pVtab, status);
  }
  return SQLITE_OK;
}

int PipelineModule::Eof(sqlite3_vtab_cursor* cursor) {
  return GetCursor(cursor)->eof;
}

int PipelineModule::Column(sqlite3_vtab_cursor* cursor,
                           sqlite3_context* ctx,
                           int raw_n) {
  Cursor* c = GetCursor(cursor);
  const Context* context = GetVtab(cursor->pVtab)->context;
  const pipeline::PipelinePlan::Column& column =
      context->plan->columns()[static_cast<uint32_t>(raw_n)];
  ResultCell(ctx, context->pool, c->rows->batch().column(column.index),
             c->rows->row());
  return SQLITE_OK;
}

int PipelineModule::Rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid) {
  *rowid = GetCursor(cursor)->rowid;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
