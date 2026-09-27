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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
#include "src/trace_processor/core/exec/memoize.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::Variant;

// The table function's arguments: the plan, then the rows of its inputs. They
// come before the outputs: SQLite only says which of a table's first 63
// columns a query reads, and those are best spent on the outputs.
constexpr int kTextColumn = 0;
constexpr int kFirstInputColumn = 1;
constexpr int kFirstOutputColumn =
    kFirstInputColumn + static_cast<int>(pipeline::kMaxPipelineInputs);

// idxNum: whether an output rowid is looked up, and how many inputs are given.
constexpr int kRowidLookup = 1;
constexpr int kInputCountShift = 1;

std::string Schema() {
  // Public names (which may repeat) are applied by the outer SELECT.
  std::vector<std::string> columns{"pipeline HIDDEN"};
  for (uint32_t i = 0; i < pipeline::kMaxPipelineInputs; ++i) {
    columns.push_back("i" + std::to_string(i) + " HIDDEN");
  }
  for (uint32_t i = 0; i < pipeline::kMaxPipelineColumns; ++i) {
    columns.push_back("c" + std::to_string(i));
  }
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

// Drops the plan the cursor is running and everything reading it.
void ResetPlan(PipelineModule::Cursor* c) {
  c->rows.reset();
  c->memoize.reset();
  c->plan.reset();
  c->serialized.clear();
}

// Takes the rows SQLite collected for each input, or fails on an argument
// which is not a collection of what the plan reads.
int TakeInputs(PipelineModule::Cursor* c,
               sqlite3_vtab* vtab,
               sqlite3_value** values,
               int count) {
  exec::CollectedRowsScan::Inputs inputs;
  for (int i = 0; i < count; ++i) {
    const auto* rows =
        static_cast<const std::shared_ptr<const exec::CollectedRows>*>(
            sqlite3_value_pointer(values[i], exec::kCollectedRowsPointerType));
    if (!rows) {
      return sqlite::utils::SetError(
          vtab, "__intrinsic_pipeline: expected collected rows");
    }
    inputs.push_back(*rows);
  }
  if (base::Status status = c->plan->CheckInputs(inputs); !status.ok()) {
    return sqlite::utils::SetError(
        vtab, base::ErrStatus("__intrinsic_pipeline: %s", status.c_message()));
  }
  // Different rows give different output, so nothing kept can be replayed.
  if (inputs != c->inputs) {
    c->rows.reset();
    c->memoize.reset();
    c->inputs = std::move(inputs);
  }
  return SQLITE_OK;
}

// Surfaces the executor's error, if any, once rows stop.
int CheckStatus(PipelineModule::Cursor* cursor) {
  // status() walks every node so only check it once rows stop.
  base::Status status = cursor->rows->status();
  return status.ok() ? SQLITE_OK
                     : sqlite::utils::SetError(cursor->pVtab, status);
}

}  // namespace

void CollectRows::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  AggCtx& agg = AggCtx::GetOrCreateContextForStep(ctx);
  if (!agg.status.ok()) {
    return;
  }
  if (!agg.rows) {
    const auto* text =
        reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    base::StatusOr<core::Schema> columns = pipeline::ReadCollectedColumns(
        text ? std::string_view(text) : std::string_view());
    if (columns.ok() && columns->size() != static_cast<size_t>(argc - 1)) {
      columns = base::ErrStatus("%s: expected %zu values, not %d", kName,
                                columns->size(), argc - 1);
    }
    if (!columns.ok()) {
      agg.status = columns.status();
      return sqlite::result::Error(ctx, agg.status.c_message());
    }
    agg.rows = std::make_shared<exec::CollectedRows>(std::move(*columns),
                                                     GetUserData(ctx));
  }
  agg.status = agg.rows->Append(argv + 1);
  if (!agg.status.ok()) {
    return sqlite::result::Error(ctx, agg.status.c_message());
  }
}

void CollectRows::Final(sqlite3_context* ctx) {
  auto agg = AggCtx::GetContextOrNullForFinal(ctx);
  std::shared_ptr<const exec::CollectedRows> rows;
  if (agg.get() && agg.get()->rows) {
    rows = std::move(agg.get()->rows);
  } else {
    // No rows, so nothing needs their columns.
    rows =
        std::make_shared<exec::CollectedRows>(core::Schema(), GetUserData(ctx));
  }
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<std::shared_ptr<const exec::CollectedRows>>(
          std::move(rows)),
      exec::kCollectedRowsPointerType);
}

int PipelineModule::Connect(sqlite3* db,
                            void* raw_ctx,
                            int,
                            const char* const*,
                            sqlite3_vtab** vtab,
                            char**) {
  std::string schema = Schema();
  if (int r = sqlite3_declare_vtab(db, schema.c_str()); r != SQLITE_OK)
    return r;
  auto res = std::make_unique<Vtab>();
  res->context = GetContext(raw_ctx);
  *vtab = res.release();
  return SQLITE_OK;
}

int PipelineModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  return SQLITE_OK;
}

int PipelineModule::BestIndex(sqlite3_vtab*, sqlite3_index_info* info) {
  int text = -1;
  int rowid = -1;
  // The constraint giving each input's rows, by position.
  std::vector<int> inputs(pipeline::kMaxPipelineInputs, -1);
  int input_count = 0;
  for (int i = 0; i < info->nConstraint; ++i) {
    const auto& constraint = info->aConstraint[i];
    if (constraint.op != SQLITE_INDEX_CONSTRAINT_EQ)
      continue;
    // Without its plan and inputs there is nothing to run, so no plan which
    // lacks them can be usable.
    if (constraint.iColumn == kTextColumn) {
      if (!constraint.usable)
        return SQLITE_CONSTRAINT;
      text = i;
    }
    if (constraint.iColumn >= kFirstInputColumn &&
        constraint.iColumn < kFirstOutputColumn) {
      if (!constraint.usable)
        return SQLITE_CONSTRAINT;
      int position = constraint.iColumn - kFirstInputColumn;
      inputs[static_cast<size_t>(position)] = i;
      input_count = std::max(input_count, position + 1);
    }
    if (constraint.iColumn == -1 && constraint.usable)
      rowid = i;
  }
  if (text == -1)
    return SQLITE_CONSTRAINT;
  info->aConstraintUsage[text].argvIndex = 1;
  info->aConstraintUsage[text].omit = true;
  for (int position = 0; position < input_count; ++position) {
    int i = inputs[static_cast<size_t>(position)];
    // Arguments are given in order, so none can be missing before the last.
    if (i == -1)
      return SQLITE_CONSTRAINT;
    info->aConstraintUsage[i].argvIndex = 2 + position;
    info->aConstraintUsage[i].omit = true;
  }
  info->idxNum = input_count << kInputCountShift;
  if (rowid != -1) {
    info->aConstraintUsage[rowid].argvIndex = 2 + input_count;
    // SQLite rechecks comparisons, including non-integer RHS values.
    info->idxNum |= kRowidLookup;
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
  int input_count = idx_num >> kInputCountShift;
  bool rowid_lookup = idx_num & kRowidLookup;
  PERFETTO_DCHECK(argc == 1 + input_count + (rowid_lookup ? 1 : 0));
  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return sqlite::utils::SetError(cursor->pVtab,
                                   "__intrinsic_pipeline: expected a plan");
  }
  std::string_view serialized(
      static_cast<const char*>(sqlite3_value_blob(argv[0])),
      static_cast<size_t>(sqlite3_value_bytes(argv[0])));
  if (!c->plan || c->serialized != serialized) {
    Context* context = GetVtab(cursor->pVtab)->context;
    auto plan = context->connection->LoadPipeline(serialized, c->inputs);
    if (!plan.ok()) {
      return sqlite::utils::SetError(cursor->pVtab, plan.status());
    }
    ResetPlan(c);
    c->plan = std::move(*plan);
    c->serialized = serialized;
    c->pool = context->pool;
  }
  if (int rc = TakeInputs(c, cursor->pVtab, argv + 1, input_count);
      rc != SQLITE_OK) {
    return rc;
  }
  // SQLite reads the inner side of a join again for each outer row. The
  // second read of a plan keeps what it produces, and later reads replay it.
  if (!c->rows) {
    c->rows = std::make_unique<core::exec::RowCursor>(c->plan->source());
  } else if (!c->memoize) {
    c->rows.reset();
    c->memoize = std::make_unique<core::exec::Memoize>(c->plan->source());
    c->rows = std::make_unique<core::exec::RowCursor>(*c->memoize);
  }
  c->rowid = 0;
  c->target_rowid.reset();
  if (rowid_lookup &&
      sqlite3_value_type(argv[1 + input_count]) == SQLITE_INTEGER) {
    c->target_rowid = sqlite3_value_int64(argv[1 + input_count]);
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
  // The text is only an argument: it is not read back.
  if (raw_n == kTextColumn) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }
  auto n = static_cast<uint32_t>(raw_n - kFirstOutputColumn);
  if (n >= c->columns.size()) {
    return sqlite::utils::SetError(
        cursor->pVtab,
        base::ErrStatus("__intrinsic_pipeline: the pipeline has no column c%u",
                        n));
  }
  const auto& column = c->columns[n];
  column.result(ctx, c->pool, *column.view, c->rows->row());
  return SQLITE_OK;
}

int PipelineModule::Rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid) {
  *rowid = GetCursor(cursor)->rowid;
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
