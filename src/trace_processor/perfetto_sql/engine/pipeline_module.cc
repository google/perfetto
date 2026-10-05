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
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/filter_pushdown.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
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

// idxNum: whether dataframes are given, and whether constraints on the output
// are pushed into the plan, which idxStr then holds. The bits from
// kListParamShift say which pushed constraints are passed a whole IN list
// rather than one value, by the parameter they bind.
constexpr int kHasDataframes = 1;
constexpr int kPushedConstraints = 2;
constexpr int kListParamShift = 2;
constexpr uint32_t kMaxListParams = 31 - kListParamShift;

// The filter a constraint SQLite passes on an output column is, if the
// pipeline can apply it exactly as SQLite would.
std::optional<core::Op> ConstraintOp(unsigned char op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return core::Eq{};
    case SQLITE_INDEX_CONSTRAINT_NE:
      return core::Ne{};
    case SQLITE_INDEX_CONSTRAINT_LT:
      return core::Lt{};
    case SQLITE_INDEX_CONSTRAINT_LE:
      return core::Le{};
    case SQLITE_INDEX_CONSTRAINT_GT:
      return core::Gt{};
    case SQLITE_INDEX_CONSTRAINT_GE:
      return core::Ge{};
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return core::IsNull{};
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return core::IsNotNull{};
    default:
      return std::nullopt;
  }
}

// Sets `slot` to a value SQLite passes for a pushed constraint, reusing the
// storage a string there already has.
base::Status SetValue(sqlite3_value* value, core::exec::Filter::Value& slot) {
  switch (sqlite3_value_type(value)) {
    case SQLITE_INTEGER:
      slot = sqlite3_value_int64(value);
      return base::OkStatus();
    case SQLITE_FLOAT:
      slot = sqlite3_value_double(value);
      return base::OkStatus();
    case SQLITE_TEXT: {
      const auto* text =
          reinterpret_cast<const char*>(sqlite3_value_text(value));
      auto size = static_cast<size_t>(sqlite3_value_bytes(value));
      switch (slot.index()) {
        case base::variant_index<core::exec::Filter::Value, std::string>():
          base::unchecked_get<std::string>(slot).assign(text, size);
          break;
        default:
          slot = std::string(text, size);
          break;
      }
      return base::OkStatus();
    }
    default:
      return base::ErrStatus(
          "__intrinsic_pipeline: cannot compare a column with a blob");
  }
}

// Sets `param` to what SQLite passes for a pushed constraint: one value, or
// with `list`, every value of an IN list. As in SQL, a null equals nothing: a
// null value is a null parameter, and a null in a list is left out. Reuses the
// parameter's storage.
base::Status SetParam(sqlite3_value* value,
                      bool list,
                      core::exec::Filter::Param& param) {
  if (!list && sqlite3_value_type(value) == SQLITE_NULL) {
    param.reset();
    return base::OkStatus();
  }
  if (!param) {
    param.emplace();
  }
  std::vector<core::exec::Filter::Value>& values = *param;
  size_t count = 0;
  auto set = [&values, &count](sqlite3_value* v) {
    if (count == values.size()) {
      values.emplace_back();
    }
    return SetValue(v, values[count++]);
  };
  if (!list) {
    RETURN_IF_ERROR(set(value));
  } else {
    sqlite3_value* item = nullptr;
    int rc = sqlite3_vtab_in_first(value, &item);
    for (; rc == SQLITE_OK && item; rc = sqlite3_vtab_in_next(value, &item)) {
      if (sqlite3_value_type(item) != SQLITE_NULL) {
        RETURN_IF_ERROR(set(item));
      }
    }
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
      return base::ErrStatus("__intrinsic_pipeline: cannot read an IN list");
    }
  }
  values.erase(values.begin() + static_cast<ptrdiff_t>(count), values.end());
  return base::OkStatus();
}

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

void ResultNull(sqlite3_context* ctx,
                StringPool*,
                const ColumnView&,
                uint32_t) {
  sqlite::result::Null(ctx);
}

void ResultNoColumn(sqlite3_context* ctx,
                    StringPool*,
                    const ColumnView&,
                    uint32_t) {
  sqlite::utils::SetError(ctx, "__intrinsic_pipeline: no such column");
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
  const auto& columns = c->plan->columns();
  for (uint32_t i = 0; i < columns.size(); ++i) {
    const auto& view = c->rows->batch().column(columns[i].index);
    c->columns[kFirstOutputColumn + i] = {&view, ReaderFor(view)};
  }
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
                            const char* idx_str,
                            sqlite3_value** argv) {
  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return sqlite::utils::SetError(c->pVtab,
                                   "__intrinsic_pipeline: expected a plan");
  }
  std::string_view serialized(
      static_cast<const char*>(sqlite3_value_blob(argv[0])),
      static_cast<size_t>(sqlite3_value_bytes(argv[0])));
  // With constraints pushed in, the plan to run is the one BestIndex made.
  std::optional<std::string> pushed;
  if (idx_num & kPushedConstraints) {
    pushed = base::Base64Decode(base::StringView(idx_str));
    if (!pushed) {
      return sqlite::utils::SetError(c->pVtab,
                                     "__intrinsic_pipeline: malformed plan");
    }
    serialized = *pushed;
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
  auto plan = context->connection->LoadPipeline(serialized, inputs);
  if (!plan.ok()) {
    return sqlite::utils::SetError(c->pVtab, plan.status());
  }
  // The rows read the old plan.
  c->rows.reset();
  c->plan = std::move(*plan);
  c->idx_str = idx_str;
  c->pool = context->pool;
  c->rows = std::make_unique<core::exec::RowCursor>(c->plan->source());
  // One reader per declared column, so Column only indexes: arguments read as
  // null, and columns past the plan's outputs fail.
  c->columns.assign(kFirstOutputColumn + pipeline::kMaxPipelineColumns,
                    {&c->no_view, &ResultNoColumn});
  for (int i = 0; i < kFirstOutputColumn; ++i) {
    c->columns[static_cast<uint32_t>(i)] = {&c->no_view, &ResultNull};
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

int PipelineModule::BestIndex(sqlite3_vtab* vtab, sqlite3_index_info* info) {
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
  // Without the plan, SQLite checks every constraint on the output itself.
  info->estimatedCost = 1e9;
  // The plan is almost always written into the SQL, so SQLite can show it to
  // us now. Then the constraints on the output are pushed into it, and the
  // query around the pipeline is planned on what running that takes.
  sqlite3_value* plan_value = nullptr;
  if (sqlite3_vtab_rhs_value(info, plan, &plan_value) != SQLITE_OK ||
      sqlite3_value_type(plan_value) != SQLITE_BLOB) {
    return SQLITE_OK;
  }
  base::StatusOr<pipeline::LogicalPlan> logical = pipeline::ParsePlan(
      std::string_view(static_cast<const char*>(sqlite3_value_blob(plan_value)),
                       static_cast<size_t>(sqlite3_value_bytes(plan_value))));
  if (!logical.ok()) {
    // Filter reports the error.
    return SQLITE_OK;
  }
  pipeline::op::Filter filter;
  uint32_t params = 0;
  for (int i = 0; i < info->nConstraint; ++i) {
    const auto& constraint = info->aConstraint[i];
    int output = constraint.iColumn - kFirstOutputColumn;
    std::optional<core::Op> op = ConstraintOp(constraint.op);
    if (!constraint.usable || output < 0 ||
        static_cast<size_t>(output) >= logical->output.size() || !op) {
      continue;
    }
    pipeline::op::FilterCondition condition;
    condition.column = logical->output[static_cast<size_t>(output)].id;
    condition.op = *op;
    // Every pushed constraint is passed a value, which a null test ignores.
    uint32_t param = params++;
    // An equality SQLite makes of an IN takes the whole list at once, so the
    // plan runs once for it rather than once for each value.
    if (op->Is<core::Eq>() && param < kMaxListParams &&
        sqlite3_vtab_in(info, i, -1)) {
      sqlite3_vtab_in(info, i, 1);
      condition.op = core::In{};
      info->idxNum |= 1 << (kListParamShift + param);
    }
    if (!op->Is<core::IsNull>() && !op->Is<core::IsNotNull>()) {
      condition.values.push_back(pipeline::op::FilterParam{param});
    }
    filter.conditions.push_back(std::move(condition));
    info->aConstraintUsage[i].argvIndex = ++argc;
    // The pipeline applies it as SQLite would, so SQLite need not again.
    info->aConstraintUsage[i].omit = true;
  }
  if (!filter.conditions.empty()) {
    logical->AddNode(std::move(filter), {logical->root});
    pipeline::PushDownFilters(*logical);
    std::string pushed =
        base::Base64Encode(base::StringView(pipeline::SerializePlan(*logical)));
    info->idxStr = sqlite3_mprintf("%s", pushed.c_str());
    info->needToFreeIdxStr = true;
    info->idxNum |= kPushedConstraints;
  }
  pipeline::PlanEstimate estimate =
      GetVtab(vtab)->context->connection->EstimatePipeline(*logical);
  info->estimatedCost = estimate.cost;
  info->estimatedRows = static_cast<sqlite3_int64>(estimate.rows.estimated);
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
                           const char* idx_str,
                           int argc,
                           sqlite3_value** argv) {
  Cursor* c = GetCursor(cursor);
  // The plan and its dataframes are the same on every filter, so it is loaded
  // once per cursor, and again only for a pushed plan BestIndex made anew.
  if (PERFETTO_UNLIKELY(!c->plan || c->idx_str != idx_str)) {
    if (int rc = Load(c, idx_num, idx_str, argv); rc != SQLITE_OK) {
      return rc;
    }
  }
  if (idx_num & kPushedConstraints) {
    bool has_dataframes = idx_num & kHasDataframes;
    core::exec::Filter::Params& params = c->plan->params();
    params.resize(static_cast<size_t>(argc - 1 - has_dataframes));
    for (uint32_t i = 0; i < params.size(); ++i) {
      bool list =
          i < kMaxListParams && (idx_num & (1 << (kListParamShift + i)));
      base::Status status =
          SetParam(argv[1 + has_dataframes + i], list, params[i]);
      if (!status.ok()) {
        return sqlite::utils::SetError(cursor->pVtab, status);
      }
    }
  }
  c->eof = !c->rows->Open();
  if (c->eof)
    return CheckStatus(c);
  CacheColumnReaders(c);
  return SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
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
                           int n) {
  Cursor* c = GetCursor(cursor);
  const auto& column = c->columns[static_cast<uint32_t>(n)];
  column.result(ctx, c->pool, *column.view, c->rows->row());
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
