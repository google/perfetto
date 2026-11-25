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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/iterator.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

// The pointer types of what __intrinsic_dataframe_agg and
// __intrinsic_dataframes give.
constexpr char kDataframePointerType[] = "PIPELINE_DATAFRAME";
constexpr char kDataframesPointerType[] = "PIPELINE_DATAFRAMES";

// Shared, so a cursor keeps the dataframes it loaded alive.
using SharedDataframe = std::shared_ptr<const dataframe::Dataframe>;

// What __intrinsic_dataframes gives: the dataframes, in the order the statement
// numbers them, null for a relation with no rows.
struct DataframeList {
  std::vector<SharedDataframe> dataframes;
};

// The table function's arguments: the statement, then the list of dataframes it
// reads. They come before the outputs: SQLite only says which of a table's
// first 63 columns a query reads, and those are best spent on the outputs.
constexpr int kPlanColumn = 0;
constexpr int kDataframesColumn = 1;
constexpr int kFirstOutputColumn = 2;

// idxNum: whether dataframes are given.
constexpr int kHasDataframes = 1;

struct SqliteCellCallback {
  void OnCell(int64_t v) const { sqlite::result::Long(ctx, v); }
  void OnCell(double v) const { sqlite::result::Double(ctx, v); }
  void OnCell(NullTermStringView v) const {
    sqlite::result::StaticTerminatedString(ctx, v.data(), v.size());
  }
  void OnCell(std::nullptr_t) const { sqlite::result::Null(ctx); }
  void OnCell(uint32_t v) const { sqlite::result::Long(ctx, v); }
  void OnCell(int32_t v) const { sqlite::result::Long(ctx, v); }
  sqlite3_context* ctx;
};

std::string Schema() {
  // Public names (which may repeat) are applied by the outer SELECT.
  std::vector<std::string> columns{"pipeline HIDDEN", "dataframes HIDDEN"};
  for (uint32_t i = 0; i < pipeline::kMaxPipelineColumns; ++i) {
    columns.push_back("c" + std::to_string(i));
  }
  return "CREATE TABLE x(" + base::Join(columns, ", ") + ")";
}

// Surfaces the executor's error, if any, once rows stop.
int CheckStatus(PipelineModule::Cursor* cursor) {
  const base::Status& status = cursor->rows->status();
  return status.ok() ? SQLITE_OK
                     : sqlite::utils::SetError(cursor->pVtab, status);
}

// The slow path of Filter: loads the statement in `argv[0]`, reading `args`,
// into `c`.
PERFETTO_NO_INLINE int Load(PipelineModule::Cursor* c,
                            int idx_num,
                            sqlite3_value** argv) {
  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return sqlite::utils::SetError(c->pVtab,
                                   "__intrinsic_pipeline: expected a plan");
  }
  // Only alive while Load runs: once bound, the iterator shares ownership of
  // each column it reads, so nothing needs to keep the dataframes alive.
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
  auto rows = context->connection->LoadPipeline(
      std::string_view(static_cast<const char*>(sqlite3_value_blob(argv[0])),
                       static_cast<size_t>(sqlite3_value_bytes(argv[0]))),
      inputs);
  if (!rows.ok()) {
    return sqlite::utils::SetError(c->pVtab, rows.status());
  }
  c->rows.emplace(std::move(*rows));
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
  // The statement and its dataframes are the same on every filter, so load
  // once per cursor.
  if (PERFETTO_UNLIKELY(!c->rows)) {
    if (int rc = Load(c, idx_num, argv); rc != SQLITE_OK) {
      return rc;
    }
  }
  c->eof = !c->rows->Open();
  if (c->eof) {
    return CheckStatus(c);
  }
  return SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
  c->eof = !c->rows->Next();
  if (c->eof) {
    return CheckStatus(c);
  }
  return SQLITE_OK;
}

int PipelineModule::Eof(sqlite3_vtab_cursor* cursor) {
  return GetCursor(cursor)->eof;
}

int PipelineModule::Column(sqlite3_vtab_cursor* cursor,
                           sqlite3_context* ctx,
                           int n) {
  Cursor* c = GetCursor(cursor);
  // Unsigned subtraction catches both hidden columns (n < kFirstOutputColumn)
  // and out-of-range output columns in a single branch on the hot path.
  auto out_col = static_cast<uint32_t>(n - kFirstOutputColumn);
  if (PERFETTO_UNLIKELY(out_col >= c->rows->column_count())) {
    if (n < kFirstOutputColumn) {
      sqlite::result::Null(ctx);
    } else {
      sqlite::utils::SetError(ctx, "__intrinsic_pipeline: no such column");
    }
    return SQLITE_OK;
  }
  c->rows->Cell(out_col, SqliteCellCallback{ctx});
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
