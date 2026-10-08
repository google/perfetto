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
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::ColumnView;
using core::exec::Variant;

// The pointer types of what __intrinsic_dataframe_agg and
// __intrinsic_dataframes give.
constexpr char kDataframePointerType[] = "PIPELINE_DATAFRAME";
constexpr char kDataframesPointerType[] = "PIPELINE_DATAFRAMES";

// Shared, so a cursor keeps the dataframes it loaded alive.
using SharedDataframe = std::shared_ptr<const dataframe::Dataframe>;

// What __intrinsic_dataframes gives: the dataframes, in the order the plan
// numbers them, null for a relation with no rows.
struct DataframeList {
  std::vector<SharedDataframe> dataframes;
};

// The table function's arguments: the plan, then the list of dataframes it
// reads. They come before the outputs: SQLite only says which of a table's
// first 63 columns a query reads, and those are best spent on the outputs.
constexpr int kPlanColumn = 0;
constexpr int kDataframesColumn = 1;
constexpr int kFirstOutputColumn = 2;

// idxNum: whether dataframes are given.
constexpr int kHasDataframes = 1;

std::string Schema() {
  // Public names (which may repeat) are applied by the outer SELECT.
  std::vector<std::string> columns{"pipeline HIDDEN", "dataframes HIDDEN"};
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
  Variant cell = view.At<Variant>(row);
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

template <typename T>
void ResultFlat(sqlite3_context* ctx,
                StringPool* pool,
                const ColumnView& view,
                uint32_t row) {
  uint32_t index = view.Index(row);
  if (view.validity() && !view.validity()->is_set(index)) {
    return sqlite::result::Null(ctx);
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
  if (!view.IsValid(row)) {
    return sqlite::result::Null(ctx);
  }
  sqlite::result::Long(ctx, view.Index(row));
}

PipelineModule::Cursor::ResultFn ReaderFor(const ColumnView& view) {
  switch (view.kind()) {
    case ColumnView::Kind::kVariant:
      return &ResultVariant;
    case ColumnView::Kind::kSequence:
      return &ResultSequence;
    case ColumnView::Kind::kFlat:
      break;
  }
  switch (view.type().index()) {
    case core::StorageType::GetTypeIndex<core::Uint32>():
      return &ResultFlat<uint32_t>;
    case core::StorageType::GetTypeIndex<core::Int32>():
      return &ResultFlat<int32_t>;
    case core::StorageType::GetTypeIndex<core::Int64>():
      return &ResultFlat<int64_t>;
    case core::StorageType::GetTypeIndex<core::Double>():
      return &ResultFlat<double>;
    case core::StorageType::GetTypeIndex<core::String>():
      return &ResultFlat<StringPool::Id>;
    default:
      PERFETTO_FATAL("Unexpected column type");
  }
}

// Works out how `reader` reads `batch`, whose lent views are `lent`.
PERFETTO_NO_INLINE void Bind(const core::exec::RowBatch& batch,
                             const core::exec::ColumnView* lent,
                             PipelineModule::Cursor::ColumnReader* reader) {
  reader->lent = lent;
  reader->view = &batch.column(reader->index);
  reader->result = ReaderFor(*reader->view);
}

// Surfaces the executor's error, if any, once rows stop.
int CheckStatus(PipelineModule::Cursor* cursor) {
  // status() walks every node so only check it once rows stop.
  base::Status status = cursor->rows->status();
  return status.ok() ? SQLITE_OK
                     : sqlite::utils::SetError(cursor->pVtab, status);
}

// The slow path of Filter: loads the plan in `value`, reading `args`, into
// `c`.
PERFETTO_NO_INLINE int Load(PipelineModule::Cursor* c,
                            int idx_num,
                            sqlite3_value** argv) {
  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return sqlite::utils::SetError(c->pVtab,
                                   "__intrinsic_pipeline: expected a plan");
  }
  // Only alive while Load runs: once bound, the plan shares ownership of each
  // column it reads, so nothing needs to keep the dataframes alive.
  std::vector<const dataframe::Dataframe*> inputs;
  if (idx_num & kHasDataframes) {
    const auto* list =
        sqlite::value::Pointer<DataframeList>(argv[1], kDataframesPointerType);
    if (!list) {
      return sqlite::utils::SetError(
          c->pVtab, "__intrinsic_pipeline: expected dataframes");
    }
    for (const SharedDataframe& input : list->dataframes) {
      inputs.push_back(input.get());
    }
  }
  PipelineModule::Context* context = PipelineModule::GetVtab(c->pVtab)->context;
  auto plan = context->connection->LoadPipeline(
      std::string_view(static_cast<const char*>(sqlite3_value_blob(argv[0])),
                       static_cast<size_t>(sqlite3_value_bytes(argv[0]))),
      inputs);
  if (!plan.ok()) {
    return sqlite::utils::SetError(c->pVtab, plan.status());
  }
  c->plan = std::move(*plan);
  c->pool = context->pool;
  c->rows = std::make_unique<core::exec::RowCursor>(c->plan->source());
  // Where each output is in the batches. A reader is worked out the first
  // time a batch's column is read, so a batch costs nothing for the columns
  // left unread.
  const auto& outputs = c->plan->columns();
  c->columns.assign(outputs.size(), {});
  for (uint32_t i = 0; i < outputs.size(); ++i) {
    c->columns[i].index = outputs[i].index;
    // No batch has been pulled with this number.
    c->columns[i].batch = c->rows->batch_number() - 1;
  }
  return SQLITE_OK;
}

}  // namespace

void DataframesFunction::Step(sqlite3_context* ctx,
                              int argc,
                              sqlite3_value** argv) {
  auto list = std::make_unique<DataframeList>();
  list->dataframes.reserve(static_cast<size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    // A pointer reads as NULL, so only a NULL without one is no rows.
    const auto* dataframe =
        sqlite::value::Pointer<SharedDataframe>(argv[i], kDataframePointerType);
    if (!dataframe && !sqlite::value::IsNull(argv[i])) {
      return sqlite::utils::SetError(
          ctx, base::ErrStatus("%s: expected dataframes", kName));
    }
    list->dataframes.push_back(dataframe ? *dataframe : nullptr);
  }
  return sqlite::result::UniquePointer(ctx, std::move(list),
                                       kDataframesPointerType);
}

void DataframeAgg::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc < 1) {
    return sqlite::utils::SetError(
        ctx, base::ErrStatus("%s: expected column names", kName));
  }
  AggCtx& agg = AggCtx::GetOrCreateContextForStep(ctx);
  auto count = static_cast<uint32_t>(argc - 1);
  if (!agg.builder) {
    const char* text = sqlite::value::Text(argv[0]);
    std::vector<std::string> names = base::SplitString(text ? text : "", ",");
    if (names.size() != count) {
      return sqlite::utils::SetError(
          ctx, base::ErrStatus("%s: expected %zu values, not %u", kName,
                               names.size(), count));
    }
    dataframe::RuntimeDataframeBuilder::Options options;
    options.emit_auto_id = false;
    options.analyze = false;
    agg.builder.emplace(std::move(names), GetUserData(ctx), options);
  }
  base::Status status = AddSqliteValuesRow(*agg.builder, argv + 1, count);
  if (!status.ok()) {
    return sqlite::utils::SetError(ctx, kName, status);
  }
}

void DataframeAgg::Final(sqlite3_context* ctx) {
  auto agg = AggCtx::GetContextOrNullForFinal(ctx);
  if (!agg.get() || !agg.get()->builder) {
    return sqlite::result::Null(ctx);
  }
  base::StatusOr<dataframe::Dataframe> dataframe =
      std::move(*agg.get()->builder).Build();
  if (!dataframe.ok()) {
    return sqlite::utils::SetError(ctx, kName, dataframe.status());
  }
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<SharedDataframe>(
          std::make_shared<const dataframe::Dataframe>(std::move(*dataframe))),
      kDataframePointerType);
}

int PipelineModule::Connect(sqlite3* db,
                            void* raw_ctx,
                            int,
                            const char* const*,
                            sqlite3_vtab** vtab,
                            char**) {
  std::string schema = Schema();
  if (int r = sqlite3_declare_vtab(db, schema.c_str()); r != SQLITE_OK) {
    return r;
  }
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
  int plan = -1;
  int dataframes = -1;
  for (int i = 0; i < info->nConstraint; ++i) {
    const auto& constraint = info->aConstraint[i];
    if (constraint.op != SQLITE_INDEX_CONSTRAINT_EQ) {
      continue;
    }
    // Without the plan and its arguments there is nothing to run.
    if (constraint.iColumn == kPlanColumn) {
      if (!constraint.usable) {
        return SQLITE_CONSTRAINT;
      }
      plan = i;
    }
    if (constraint.iColumn == kDataframesColumn) {
      if (!constraint.usable) {
        return SQLITE_CONSTRAINT;
      }
      dataframes = i;
    }
  }
  if (plan == -1) {
    return SQLITE_CONSTRAINT;
  }
  int argc = 0;
  info->aConstraintUsage[plan].argvIndex = ++argc;
  info->aConstraintUsage[plan].omit = true;
  info->idxNum = 0;
  if (dataframes != -1) {
    info->aConstraintUsage[dataframes].argvIndex = ++argc;
    info->aConstraintUsage[dataframes].omit = true;
    info->idxNum |= kHasDataframes;
  }
  info->estimatedCost = 1e9;
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
                           int,
                           sqlite3_value** argv) {
  Cursor* c = GetCursor(cursor);
  // The plan and its dataframes are the same on every filter, so it is loaded
  // once per cursor.
  if (PERFETTO_UNLIKELY(!c->plan)) {
    if (int rc = Load(c, idx_num, argv); rc != SQLITE_OK) {
      return rc;
    }
  }
  c->eof = !c->rows->Open();
  return c->eof ? CheckStatus(c) : SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
  c->eof = !c->rows->Next();
  return c->eof ? CheckStatus(c) : SQLITE_OK;
}

int PipelineModule::Eof(sqlite3_vtab_cursor* cursor) {
  return GetCursor(cursor)->eof;
}

int PipelineModule::Column(sqlite3_vtab_cursor* cursor,
                           sqlite3_context* ctx,
                           int n) {
  Cursor* c = GetCursor(cursor);
  // The arguments read as null, and columns past the plan's outputs fail.
  auto output = static_cast<uint32_t>(n - kFirstOutputColumn);
  if (PERFETTO_UNLIKELY(output >= c->columns.size())) {
    if (n < kFirstOutputColumn) {
      sqlite::result::Null(ctx);
    } else {
      sqlite::utils::SetError(ctx, "__intrinsic_pipeline: no such column");
    }
    return SQLITE_OK;
  }
  Cursor::ColumnReader& reader = c->columns[output];
  if (PERFETTO_UNLIKELY(reader.batch != c->rows->batch_number())) {
    reader.batch = c->rows->batch_number();
    // Lent views keep their kind and type, so are read as they were.
    const core::exec::RowBatch& batch = c->rows->batch();
    const core::exec::ColumnView* lent = batch.lent_columns();
    if (!lent || lent != reader.lent) {
      Bind(batch, lent, &reader);
    }
    PERFETTO_DCHECK(reader.result == ReaderFor(*reader.view));
  }
  reader.result(ctx, c->pool, *reader.view, c->rows->row());
  return SQLITE_OK;
}

int PipelineModule::Rowid(sqlite3_vtab_cursor* cursor, sqlite_int64*) {
  // A position in one run need not be the same row in another, filtered
  // differently, so rows have no rowid rather than a misleading one.
  return sqlite::utils::SetError(cursor->pVtab,
                                 "__intrinsic_pipeline: a pipeline's rows have "
                                 "no rowid");
}

}  // namespace perfetto::trace_processor
