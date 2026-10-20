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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/runtime_dataframe_builder.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/engine/plan_cursor.h"
#include "src/trace_processor/perfetto_sql/engine/sqlite_dataframe_builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/filter_passing.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_sql.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor {
namespace {

using core::exec::RowBatch;

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

// idxStr: `<dataframes>:<plan>`. `dataframes` is 1 if the list of dataframes
// is given, after the plan, and 0 if not. If constraints on the output are
// pushed into the plan, their values come next, and `plan` is the plan to run,
// in base64; it is empty otherwise.
void SetIdxStr(sqlite3_index_info* info,
               bool dataframes,
               const std::string& plan) {
  if (info->needToFreeIdxStr) {
    sqlite3_free(info->idxStr);
  }
  info->idxStr = sqlite3_mprintf("%d:%s", dataframes, plan.c_str());
  info->needToFreeIdxStr = true;
}

// The predicate a constraint SQLite passes on an output column is, if the
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

std::string Schema() {
  // Public names (which may repeat) are applied by the outer SELECT.
  std::vector<std::string> columns{"pipeline HIDDEN", "dataframes HIDDEN"};
  for (uint32_t i = 0; i < pipeline::kMaxPipelineColumns; ++i) {
    columns.push_back("c" + std::to_string(i));
  }
  return "CREATE TABLE x(" + base::Join(columns, ", ") + ")";
}

void ResultNull(sqlite3_context* ctx,
                StringPool*,
                const RowBatch&,
                uint32_t,
                uint32_t) {
  sqlite::result::Null(ctx);
}

void ResultNoColumn(sqlite3_context* ctx,
                    StringPool*,
                    const RowBatch&,
                    uint32_t,
                    uint32_t) {
  sqlite::utils::SetError(ctx, "__intrinsic_pipeline: no such column");
}

// Surfaces the executor's error, if any, once rows stop.
int CheckStatus(PipelineModule::Cursor* cursor) {
  // status() walks every node so only check it once rows stop.
  base::Status status = cursor->plan->status();
  return status.ok() ? SQLITE_OK
                     : sqlite::utils::SetError(cursor->pVtab, status);
}

// The slow path of Filter: loads the plan in `argv`, or the one BestIndex
// pushed constraints into in `idx_str`, into `c` for `idx_num`.
PERFETTO_NO_INLINE int Load(PipelineModule::Cursor* c,
                            int idx_num,
                            const char* idx_str,
                            int argc,
                            sqlite3_value** argv) {
  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return sqlite::utils::SetError(c->pVtab,
                                   "__intrinsic_pipeline: expected a plan");
  }
  std::string_view serialized(
      static_cast<const char*>(sqlite3_value_blob(argv[0])),
      static_cast<size_t>(sqlite3_value_bytes(argv[0])));
  // With constraints pushed in, the plan to run is the one BestIndex made.
  if (!idx_str || (idx_str[0] != '0' && idx_str[0] != '1') ||
      idx_str[1] != ':') {
    return sqlite::utils::SetError(c->pVtab,
                                   "__intrinsic_pipeline: malformed plan");
  }
  bool has_dataframes = idx_str[0] == '1';
  std::optional<std::string> pushed;
  if (idx_str[2]) {
    pushed = base::Base64Decode(base::StringView(idx_str + 2));
    if (!pushed) {
      return sqlite::utils::SetError(c->pVtab,
                                     "__intrinsic_pipeline: malformed plan");
    }
    serialized = *pushed;
  }

  // Only alive while Load runs: once bound, the plan shares ownership of each
  // column it reads, so nothing needs to keep the dataframes alive.
  std::vector<const dataframe::Dataframe*> inputs;
  if (has_dataframes) {
    const auto* list = sqlite::value::Pointer<DataframeList>(
        argv[kDataframesColumn], kDataframesPointerType);
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
  uint32_t first_param =
      has_dataframes ? kDataframesColumn + 1 : kDataframesColumn;
  for (const pipeline::PhysicalPlan::Param& param : (*plan)->params()) {
    if (first_param + param.param >= static_cast<uint32_t>(argc)) {
      return sqlite::utils::SetError(
          c->pVtab, "__intrinsic_pipeline: missing a constraint's value");
    }
  }
  c->plan = std::make_unique<PlanCursor>(std::move(*plan), context->pool);
  c->idx_num = idx_num;
  c->first_param = first_param;
  // One reader per declared column, so Column only indexes: arguments read as
  // null, and columns past the plan's outputs fail. Each output is read as
  // the plan lays it out, so the same way in every batch.
  c->columns.assign(kFirstOutputColumn + pipeline::kMaxPipelineColumns,
                    {0, &ResultNoColumn});
  for (int i = 0; i < kFirstOutputColumn; ++i) {
    c->columns[static_cast<uint32_t>(i)] = {0, &ResultNull};
  }
  auto outputs = static_cast<uint32_t>(c->plan->plan().columns().size());
  for (uint32_t i = 0; i < outputs; ++i) {
    c->columns[kFirstOutputColumn + i] = c->plan->OutputReader(i);
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
  if (dataframes != -1) {
    info->aConstraintUsage[dataframes].argvIndex = ++argc;
    info->aConstraintUsage[dataframes].omit = true;
  }
  // Each plan is numbered, from 1, so a cursor knows by its idxNum alone
  // whether the plan it has loaded is the one to run.
  Vtab* v = GetVtab(vtab);
  v->plans = v->plans == std::numeric_limits<int>::max() ? 1 : v->plans + 1;
  info->idxNum = v->plans;
  SetIdxStr(info, dataframes != -1, "");
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
  std::vector<pipeline::WherePredicate> predicates;
  uint32_t params = 0;
  for (int i = 0; i < info->nConstraint; ++i) {
    const auto& constraint = info->aConstraint[i];
    int output = constraint.iColumn - kFirstOutputColumn;
    std::optional<core::Op> op = ConstraintOp(constraint.op);
    if (!constraint.usable || output < 0 ||
        static_cast<size_t>(output) >= logical->output().size() || !op) {
      continue;
    }
    pipeline::WherePredicate predicate;
    predicate.column = logical->output()[static_cast<size_t>(output)].id;
    predicate.op = *op;
    // Every pushed constraint is passed a value, which a null test ignores.
    predicate.param = params++;
    // An equality SQLite makes of an IN takes the whole list at once, so the
    // plan runs once for it rather than once for each value.
    if (op->Is<core::Eq>() && sqlite3_vtab_in(info, i, -1)) {
      sqlite3_vtab_in(info, i, 1);
      predicate.op = core::In{};
    }
    if (op->IsAnyOf<core::NullOp>()) {
      predicate.param = std::nullopt;
    }
    predicates.push_back(predicate);
    info->aConstraintUsage[i].argvIndex = ++argc;
    // The pipeline applies it as SQLite would, so SQLite need not again.
    info->aConstraintUsage[i].omit = true;
  }
  if (!predicates.empty()) {
    pipeline::Where::AddToPlan(&*logical, std::move(predicates));
    pipeline::PassFiltersThrough(*logical);
    std::string pushed =
        base::Base64Encode(base::StringView(pipeline::SerializePlan(*logical)));
    SetIdxStr(info, dataframes != -1, pushed);
  }
  pipeline::PlanEstimate estimate =
      v->context->connection->EstimatePipeline(*logical);
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
  // A plan and its dataframes are the same on every filter, so a cursor loads
  // one only when given a plan it has not loaded.
  if (PERFETTO_UNLIKELY(idx_num != c->idx_num)) {
    if (int rc = Load(c, idx_num, idx_str, argc, argv); rc != SQLITE_OK) {
      return rc;
    }
  }
  c->eof = !c->plan->Run(argv + c->first_param);
  return c->eof ? CheckStatus(c) : SQLITE_OK;
}

int PipelineModule::Next(sqlite3_vtab_cursor* cursor) {
  Cursor* c = GetCursor(cursor);
  c->eof = !c->plan->Next();
  return c->eof ? CheckStatus(c) : SQLITE_OK;
}

int PipelineModule::Eof(sqlite3_vtab_cursor* cursor) {
  return GetCursor(cursor)->eof;
}

int PipelineModule::Column(sqlite3_vtab_cursor* cursor,
                           sqlite3_context* ctx,
                           int n) {
  Cursor* c = GetCursor(cursor);
  c->plan->Result(ctx, c->columns[static_cast<uint32_t>(n)]);
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
