/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/engine/dataframe_module.h"

#include <sqlite3.h>
#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/dataframe/cursor_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/module_state_manager.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"

namespace perfetto::trace_processor {

namespace {

std::optional<dataframe::Op> SqliteOpToDataframeOp(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return dataframe::Eq();
    case SQLITE_INDEX_CONSTRAINT_NE:
      return dataframe::Ne();
    case SQLITE_INDEX_CONSTRAINT_LT:
      return dataframe::Lt();
    case SQLITE_INDEX_CONSTRAINT_LE:
      return dataframe::Le();
    case SQLITE_INDEX_CONSTRAINT_GT:
      return dataframe::Gt();
    case SQLITE_INDEX_CONSTRAINT_GE:
      return dataframe::Ge();
    case SQLITE_INDEX_CONSTRAINT_GLOB:
      return dataframe::Glob();
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return dataframe::IsNull();
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return dataframe::IsNotNull();
    default:
      return std::nullopt;
  }
}

std::string OpToString(int op) {
  switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
      return "=";
    case SQLITE_INDEX_CONSTRAINT_NE:
      return "!=";
    case SQLITE_INDEX_CONSTRAINT_GE:
      return ">=";
    case SQLITE_INDEX_CONSTRAINT_GT:
      return ">";
    case SQLITE_INDEX_CONSTRAINT_LE:
      return "<=";
    case SQLITE_INDEX_CONSTRAINT_LT:
      return "<";
    case SQLITE_INDEX_CONSTRAINT_MATCH:
      return " match ";
    case SQLITE_INDEX_CONSTRAINT_LIKE:
      return " like ";
    case SQLITE_INDEX_CONSTRAINT_GLOB:
      return " glob ";
    case SQLITE_INDEX_CONSTRAINT_REGEXP:
      return " regexp ";
    case SQLITE_INDEX_CONSTRAINT_ISNULL:
      return " is null";
    case SQLITE_INDEX_CONSTRAINT_ISNOTNULL:
      return " is not null";
    case SQLITE_INDEX_CONSTRAINT_LIMIT:
      return "limit";
    case SQLITE_INDEX_CONSTRAINT_OFFSET:
      return "offset";
    case SQLITE_INDEX_CONSTRAINT_FUNCTION:
      return "function";
    default:
      return "unknown";
  }
}

std::string ToSqliteCreateTableType(dataframe::StorageType type) {
  switch (type.index()) {
    case dataframe::StorageType::GetTypeIndex<dataframe::Id>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Uint32>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Int32>():
    case dataframe::StorageType::GetTypeIndex<dataframe::Int64>():
      return "INTEGER";
    case dataframe::StorageType::GetTypeIndex<dataframe::Double>():
      return "DOUBLE";
    case dataframe::StorageType::GetTypeIndex<dataframe::String>():
      return "TEXT";
    default:
      PERFETTO_FATAL("Unimplemented");
  }
}

uint32_t FindIdColumnIndex(const std::vector<std::string>& names) {
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (names[i] == "id" || names[i] == dataframe::kAutoIdColumnName) {
      return i;
    }
  }
  PERFETTO_FATAL("No id column found");
}

std::string CreateTableStmt(const dataframe::DataframeSpec& spec) {
  uint32_t id_col_idx = FindIdColumnIndex(spec.column_names);
  std::string create_stmt = "CREATE TABLE x(";
  for (uint32_t i = 0; i < spec.column_specs.size(); ++i) {
    create_stmt += spec.column_names[i] + " " +
                   ToSqliteCreateTableType(spec.column_specs[i].type);
    if (dataframe::IsHiddenColumn(spec.column_names[i])) {
      create_stmt += " HIDDEN";
    }
    create_stmt += ", ";
  }
  create_stmt += "PRIMARY KEY(" + spec.column_names[id_col_idx] + "))";
  return create_stmt;
}

dataframe::Op OpFromIndex(uint32_t index) {
  switch (index) {
    case dataframe::Op::GetTypeIndex<dataframe::Eq>():
      return dataframe::Eq{};
    case dataframe::Op::GetTypeIndex<dataframe::Ne>():
      return dataframe::Ne{};
    case dataframe::Op::GetTypeIndex<dataframe::Lt>():
      return dataframe::Lt{};
    case dataframe::Op::GetTypeIndex<dataframe::Le>():
      return dataframe::Le{};
    case dataframe::Op::GetTypeIndex<dataframe::Gt>():
      return dataframe::Gt{};
    case dataframe::Op::GetTypeIndex<dataframe::Ge>():
      return dataframe::Ge{};
    case dataframe::Op::GetTypeIndex<dataframe::Glob>():
      return dataframe::Glob{};
    case dataframe::Op::GetTypeIndex<dataframe::IsNull>():
      return dataframe::IsNull{};
    case dataframe::Op::GetTypeIndex<dataframe::IsNotNull>():
      return dataframe::IsNotNull{};
    case dataframe::Op::GetTypeIndex<dataframe::In>():
      return dataframe::In{};
    default:
      PERFETTO_FATAL("Not an operator the executor runs");
  }
}

// The idxStr of a query: `<columns used>|`, then `<column>,<operator>,
// <argument>;` per filter, with `-` for a filter given no argument and `L`
// before the argument of one whose value SQLite knows is constant (see
// `constant`, by constraint), then `|` and what is done with the rows kept:
// `D;` to keep one of each set equal in the columns used, `S<column>,<1 if
// descending>;` per sort key, and `L<count>;` and `O<offset>;` to limit them.
std::string ExecQueryString(
    const std::vector<dataframe::FilterSpec>& filters,
    const std::vector<bool>& constant,
    const std::vector<dataframe::DistinctSpec>& distinct,
    const std::vector<dataframe::SortSpec>& sorts,
    const dataframe::LimitSpec& limit,
    uint64_t cols_used) {
  std::string query =
      base::StackString<32>("%" PRIx64 "|", cols_used).ToStdString();
  for (const dataframe::FilterSpec& filter : filters) {
    bool null_op = filter.op.IsAnyOf<core::NullOp>();
    // SQLite tests a constraint it is not given the value of itself.
    if (!null_op && !filter.value_index) {
      continue;
    }
    query += base::StackString<64>("%u,%u,", filter.col, filter.op.index())
                 .ToStdString();
    if (null_op) {
      query += "-";
    } else {
      query += constant[filter.source_index] ? "L" : "";
      query += std::to_string(*filter.value_index);
    }
    query += ";";
  }
  query += "|";
  if (!distinct.empty()) {
    query += "D;";
  }
  for (const dataframe::SortSpec& sort : sorts) {
    query += base::StackString<32>(
                 "S%u,%d;", sort.col,
                 sort.direction == dataframe::SortDirection::kDescending)
                 .ToStdString();
  }
  if (limit.limit) {
    query += "L" + std::to_string(*limit.limit) + ";";
  }
  if (limit.offset) {
    query += "O" + std::to_string(*limit.offset) + ";";
  }
  return query;
}

void ResultUnread(sqlite3_context* ctx,
                  StringPool*,
                  const core::exec::ColumnView&,
                  uint32_t) {
  sqlite::result::Null(ctx);
}

using ExecPlan = DataframeModule::ExecPlan;
using ExecQuery = DataframeModule::ExecQuery;
base::StatusOr<std::shared_ptr<const ExecPlan>> LoadExecPlan(
    const dataframe::Dataframe& df,
    const std::string& name,
    uint32_t id_column,
    const char* query) {
  char* end;
  uint64_t cols_used = strtoull(query, &end, 16);
  PERFETTO_CHECK(*end == '|');
  struct Spec {
    uint32_t column = 0;
    dataframe::Op op{dataframe::Eq{}};
    std::optional<uint32_t> arg;
    // Whether `arg` is the same for every run of the statement.
    bool constant = false;
  };
  std::vector<Spec> specs;
  char* it = end + 1;
  for (; *it != '|';) {
    Spec spec;
    spec.column = static_cast<uint32_t>(strtoul(it, &end, 10));
    spec.op = OpFromIndex(static_cast<uint32_t>(strtoul(end + 1, &end, 10)));
    if (end[1] == '-') {
      end += 2;
    } else {
      spec.constant = end[1] == 'L';
      spec.arg = static_cast<uint32_t>(
          strtoul(end + 1 + (spec.constant ? 1 : 0), &end, 10));
    }
    PERFETTO_CHECK(*end == ';');
    it = end + 1;
    specs.push_back(spec);
  }
  bool distinct = false;
  std::vector<pipeline::SortKey> sorts;
  std::vector<uint32_t> sort_columns;
  int64_t limit = -1;
  int64_t offset = 0;
  for (++it; *it; it = end + 1) {
    char kind = *it;
    switch (kind) {
      case 'D':
        distinct = true;
        end = it + 1;
        break;
      case 'S': {
        sort_columns.push_back(
            static_cast<uint32_t>(strtoul(it + 1, &end, 10)));
        pipeline::SortKey key;
        key.descending = strtoul(end + 1, &end, 10) != 0;
        sorts.push_back(key);
        break;
      }
      case 'L':
        limit = static_cast<int64_t>(strtoull(it + 1, &end, 10));
        break;
      case 'O':
        offset = static_cast<int64_t>(strtoull(it + 1, &end, 10));
        break;
      default:
        PERFETTO_FATAL("Unknown part of a query string");
    }
    PERFETTO_CHECK(*end == ';');
  }

  // The last bit of colUsed stands for every column from it on. The id is the
  // rowid, so it is always read.
  uint32_t count = df.column_count();
  std::vector<bool> output(count);
  for (uint32_t i = 0; i < count; ++i) {
    output[i] = (cols_used >> std::min(i, 63u)) & 1;
  }
  output[id_column] = true;
  std::vector<bool> read = output;
  for (const Spec& spec : specs) {
    read[spec.column] = true;
  }
  for (uint32_t column : sort_columns) {
    read[column] = true;
  }
  std::vector<uint32_t> columns;
  std::vector<uint32_t> position(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (read[i]) {
      position[i] = static_cast<uint32_t>(columns.size());
      columns.push_back(i);
    }
  }

  pipeline::LogicalPlan plan;
  pipeline::PlanNodeId scan =
      pipeline::Scan::AddToPlan(&plan, df, name, columns);
  std::vector<pipeline::NamedColumn> scanned =
      plan.nodes()[scan].Cast<pipeline::Scan>().columns();
  std::vector<pipeline::WherePredicate> predicates;
  std::vector<pipeline::Literal> literals;
  for (const Spec& spec : specs) {
    pipeline::WherePredicate predicate;
    predicate.column = scanned[position[spec.column]].id;
    predicate.op = spec.op;
    if (spec.arg) {
      predicate.first_literal = static_cast<uint32_t>(literals.size());
      predicate.literal_count = 1;
      literals.push_back(spec.constant ? pipeline::Literal::External(*spec.arg)
                                       : pipeline::Literal::Bound(*spec.arg));
    }
    predicates.push_back(predicate);
  }
  if (!predicates.empty()) {
    pipeline::Where::AddToPlan(&plan, std::move(predicates),
                               std::move(literals));
  }
  if (distinct) {
    // As SQLite allows, one row of each set equal in every column it reads.
    std::vector<pipeline::ColumnId> keys;
    for (uint32_t i = 0; i < std::min(count, 64u); ++i) {
      if ((cols_used >> i) & 1) {
        keys.push_back(scanned[position[i]].id);
      }
    }
    pipeline::Distinct::AddToPlan(&plan, std::move(keys));
  }
  if (!sorts.empty()) {
    for (uint32_t i = 0; i < sorts.size(); ++i) {
      sorts[i].column = scanned[position[sort_columns[i]]].id;
    }
    pipeline::OrderBy::AddToPlan(&plan, std::move(sorts));
  }
  if (limit >= 0 || offset > 0) {
    pipeline::Limit::AddToPlan(&plan, limit, offset);
  }
  std::vector<uint32_t> outputs;
  for (uint32_t i = 0; i < count; ++i) {
    if (output[i]) {
      outputs.push_back(i);
      plan.output().push_back(scanned[position[i]]);
    }
  }

  pipeline::BoundPlan bound(std::move(plan));
  pipeline::Scan::Bind(&bound, scan, df);
  pipeline::LoweringContext context;
  context.pool = df.string_pool();
  context.sparse_outputs = pipeline::LoweringContext::SparseOutputs::kLend;
  context.sorted_outputs = pipeline::LoweringContext::SortedOutputs::kInPlace;
  RETURN_IF_ERROR(pipeline::RunBoundPasses(&bound, context));
  auto q = std::make_shared<ExecPlan>();
  q->plan = pipeline::Lower(&bound, context);
  RETURN_IF_ERROR(bound.status);
  q->pool = df.string_pool();
  q->columns.assign(count, ExecPlan::kUnread);
  const auto& plan_columns = q->plan->columns();
  for (uint32_t k = 0; k < outputs.size(); ++k) {
    q->columns[outputs[k]] = plan_columns[k].index;
  }
  q->id_index = q->columns[id_column];
  return std::shared_ptr<const ExecPlan>(std::move(q));
}

}  // namespace

int DataframeModule::Create(sqlite3* db,
                            void* raw_ctx,
                            int argc,
                            const char* const* argv,
                            sqlite3_vtab** vtab,
                            char** err) {
  PERFETTO_CHECK(argc == 3);

  auto* ctx = GetContext(raw_ctx);
  auto state = std::move(ctx->temporary_create_state);
  PERFETTO_CHECK(state);

  std::string create_stmt = CreateTableStmt(state->dataframe->CreateSpec());
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK) {
    *err = sqlite3_mprintf("failed to declare vtab %s", create_stmt.c_str());
    return r;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->id_col_idx = FindIdColumnIndex(state->dataframe->column_names());
  res->state = ctx->OnCreate(argc, argv, std::move(state));
  res->name = argv[2];
  *vtab = res.release();
  return SQLITE_OK;
}

int DataframeModule::Destroy(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  sqlite::ModuleStateManager<DataframeModule>::OnDestroy(v->state);
  return SQLITE_OK;
}

int DataframeModule::Connect(sqlite3* db,
                             void* raw_ctx,
                             int argc,
                             const char* const* argv,
                             sqlite3_vtab** vtab,
                             char** pzErr) {
  PERFETTO_CHECK(argc == 3);

  auto* vtab_state = GetContext(raw_ctx)->OnConnect(argc, argv);
  if (!vtab_state) {
    *pzErr = sqlite3_mprintf(
        "table '%s' could not be connected to because it could not be found",
        argv[2]);
    return SQLITE_ERROR;
  }
  auto* state =
      sqlite::ModuleStateManager<DataframeModule>::GetState(vtab_state);
  std::string create_stmt = CreateTableStmt(state->dataframe->CreateSpec());
  if (int r = sqlite3_declare_vtab(db, create_stmt.c_str()); r != SQLITE_OK) {
    return r;
  }
  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->state = vtab_state;
  res->id_col_idx = FindIdColumnIndex(state->dataframe->column_names());
  res->name = argv[2];
  *vtab = res.release();
  return SQLITE_OK;
}

int DataframeModule::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> v(GetVtab(vtab));
  return SQLITE_OK;
}

int DataframeModule::BestIndex(sqlite3_vtab* tab, sqlite3_index_info* info) {
  auto* v = GetVtab(tab);
  auto* s = sqlite::ModuleStateManager<DataframeModule>::GetState(v->state);

  std::optional<int> limit_constraint_idx;
  std::optional<int> offset_constraint_idx;

  std::vector<dataframe::FilterSpec> filter_specs;
  // By constraint, whether SQLite knows its value before running.
  std::vector<bool> constant(static_cast<size_t>(info->nConstraint));
  dataframe::LimitSpec limit_spec;
  filter_specs.reserve(static_cast<size_t>(info->nConstraint));
  bool has_unknown_constraint = false;
  for (int i = 0; i < info->nConstraint; ++i) {
    if (!info->aConstraint[i].usable) {
      continue;
    }
    sqlite3_value* rhs;
    int ret = sqlite3_vtab_rhs_value(info, i, &rhs);
    PERFETTO_CHECK(ret == SQLITE_OK || ret == SQLITE_NOTFOUND);

    int op = info->aConstraint[i].op;

    // Special handling for limit/offset values when we have a constant value.
    bool is_limit_offset = op == SQLITE_INDEX_CONSTRAINT_LIMIT ||
                           op == SQLITE_INDEX_CONSTRAINT_OFFSET;
    if (is_limit_offset && rhs &&
        sqlite::value::Type(rhs) == sqlite::Type::kInteger) {
      int64_t value = sqlite::value::Int64(rhs);
      if (value >= 0 && value <= std::numeric_limits<uint32_t>::max()) {
        auto cast = static_cast<uint32_t>(value);
        if (op == SQLITE_INDEX_CONSTRAINT_LIMIT) {
          limit_spec.limit = cast;
          limit_constraint_idx = i;
        } else {
          PERFETTO_DCHECK(op == SQLITE_INDEX_CONSTRAINT_OFFSET);
          limit_spec.offset = cast;
          offset_constraint_idx = i;
        }
        continue;
      }
    }
    auto df_op = SqliteOpToDataframeOp(op);
    if (!df_op) {
      has_unknown_constraint = true;
      continue;
    }
    // Convert the eq constraint to a Is constraint if possible.
    if (df_op->Is<dataframe::Eq>() && sqlite3_vtab_in(info, i, -1)) {
      df_op = dataframe::In();
      PERFETTO_CHECK(sqlite3_vtab_in(info, i, 1));
    }
    constant[static_cast<size_t>(i)] = rhs && !df_op->Is<dataframe::In>();
    filter_specs.emplace_back(dataframe::FilterSpec{
        static_cast<uint32_t>(info->aConstraint[i].iColumn),
        static_cast<uint32_t>(i),
        *df_op,
        std::nullopt,
    });
  }

  // If we have a constraint we don't understand, we should ignore the limit
  // and offset constraints.
  if (has_unknown_constraint) {
    limit_spec = dataframe::LimitSpec{};
    limit_constraint_idx = std::nullopt;
    offset_constraint_idx = std::nullopt;
  }

  bool should_sort_using_order_by = true;
  std::vector<dataframe::DistinctSpec> distinct_specs;
  if (info->nOrderBy > 0) {
    int vtab_distinct = sqlite3_vtab_distinct(info);
    switch (vtab_distinct) {
      case 0: /* normal sorting */
      // TODO(lalitm): add special handling for group by.
      case 1: /* group by */
        break;
      case 2: /* distinct */
      case 3: /* distinct + order by */ {
        // The contract lets us dedup over the colUsed columns.
        uint64_t cols_used_it = info->colUsed;
        for (uint32_t i = 0; i < 64; ++i) {
          if (cols_used_it & 1u) {
            distinct_specs.push_back(dataframe::DistinctSpec{i});
          }
          cols_used_it >>= 1;
        }
        // SQLite's DISTINCT handling for virtual tables changed between 3.50.3
        // and 3.53. Newer SQLite trusts
        // orderByConsumed and, for a plain DISTINCT, performs only an
        // adjacent-row dedup (WHERE_DISTINCT_ORDERED) assuming we grouped equal
        // rows; deduping over colUsed alone does not do that, so we must sort
        // by aOrderBy ourselves. Older SQLite re-dedups regardless, so we only
        // sort when there is also an ORDER BY to satisfy (vtab_distinct == 3).
#if SQLITE_VERSION_NUMBER > 3050300
        should_sort_using_order_by = true;
#else
        should_sort_using_order_by = (vtab_distinct == 3);
#endif
        break;
      }
      default:
        PERFETTO_FATAL("Unreachable");
    }
  }

  std::vector<dataframe::SortSpec> sort_specs;
  if (should_sort_using_order_by) {
    sort_specs.reserve(static_cast<size_t>(info->nOrderBy));
    for (int i = 0; i < info->nOrderBy; ++i) {
      sort_specs.emplace_back(dataframe::SortSpec{
          static_cast<uint32_t>(info->aOrderBy[i].iColumn),
          info->aOrderBy[i].desc ? dataframe::SortDirection::kDescending
                                 : dataframe::SortDirection::kAscending});
    }
  }
  info->orderByConsumed = true;

  SQLITE_ASSIGN_OR_RETURN(
      tab, auto plan,
      s->dataframe->PlanQuery(filter_specs, distinct_specs, sort_specs,
                              limit_spec, info->colUsed));
  int max_argv = 0;
  for (const auto& c : filter_specs) {
    if (auto value_index = c.value_index; value_index) {
      info->aConstraintUsage[c.source_index].argvIndex =
          static_cast<int>(*value_index) + 1;
      info->aConstraintUsage[c.source_index].omit = true;
      max_argv =
          std::max(max_argv, info->aConstraintUsage[c.source_index].argvIndex);
    }
  }
  if (limit_constraint_idx) {
    info->aConstraintUsage[*limit_constraint_idx].omit = true;
    info->aConstraintUsage[*limit_constraint_idx].argvIndex = ++max_argv;
  }
  if (offset_constraint_idx) {
    info->aConstraintUsage[*offset_constraint_idx].omit = true;
    info->aConstraintUsage[*offset_constraint_idx].argvIndex = ++max_argv;
  }
  info->needToFreeIdxStr = true;
  info->estimatedCost = plan.estimated_cost();
  info->estimatedRows = plan.estimated_row_count();
  if (plan.max_row_count() <= 1) {
    info->idxFlags |= SQLITE_INDEX_SCAN_UNIQUE;
  }
  info->idxNum = v->best_idx_num++;
  PERFETTO_TP_TRACE(
      metatrace::Category::QUERY_TIMELINE, "DATAFRAME_BEST_INDEX",
      [info, v, s, &plan](metatrace::Record* record) {
        base::StackString<32> unique("%d",
                                     info->idxFlags & SQLITE_INDEX_SCAN_UNIQUE);
        record->AddArg("name", v->name);
        record->AddArg("unique", unique.string_view());
        record->AddArg("idxNum",
                       base::StackString<32>("%d", info->idxNum).string_view());
        record->AddArg(
            "estimatedCost",
            base::StackString<32>("%f", info->estimatedCost).string_view());
        record->AddArg(
            "estimatedRows",
            base::StackString<32>("%lld", info->estimatedRows).string_view());
        record->AddArg(
            "orderByConsumed",
            base::StackString<32>("%d", info->orderByConsumed).string_view());
        for (uint64_t u = info->colUsed, i = 0, j = 0; u != 0; u >>= 1, ++i) {
          if (u & 1) {
            base::StackString<32> c("colUsed[%" PRIu64 "]", j++);
            record->AddArg(c.string_view(), s->dataframe->column_names()[i]);
          }
        }
        auto str = plan.BytecodeToString();
        for (uint32_t i = 0; i < str.size(); ++i) {
          base::StackString<32> c("bytecode[%u]", i);
          record->AddArg(c.string_view(), str[i]);
        }
        for (int i = 0, j = 0; i < info->nConstraint; ++i) {
          if (!info->aConstraint[i].usable) {
            continue;
          }
          {
            base::StackString<32> c("constraint[%d].column", j);
            auto col_idx = static_cast<uint32_t>(info->aConstraint[i].iColumn);
            record->AddArg(c.string_view(),
                           s->dataframe->column_names()[col_idx]);
          }
          {
            base::StackString<32> c("constraint[%d].op", j);
            record->AddArg(c.string_view(),
                           OpToString(info->aConstraint[i].op));
          }
          {
            base::StackString<32> c("constraint[%d].argvIndex", j);
            record->AddArg(c.string_view(),
                           std::to_string(info->aConstraintUsage[i].argvIndex));
          }
          {
            base::StackString<32> c("constraint[%d].omit", j);
            record->AddArg(c.string_view(),
                           std::to_string(info->aConstraintUsage[i].omit));
          }
          {
            base::StackString<32> c("constraint[%d].in", j);
            record->AddArg(c.string_view(),
                           std::to_string(sqlite3_vtab_in(info, i, -1)));
          }
          j++;
        }
        for (int i = 0; i < info->nOrderBy; ++i) {
          {
            base::StackString<32> c("order_by[%d].column", i);
            auto col_idx = static_cast<uint32_t>(info->aOrderBy[i].iColumn);
            record->AddArg(c.string_view(),
                           s->dataframe->column_names()[col_idx]);
          }
          {
            base::StackString<32> c("order_by[%d].desc", i);
            record->AddArg(c.string_view(),
                           std::to_string(info->aOrderBy[i].desc));
          }
        }
      });
  info->idxStr = sqlite3_mprintf(
      "%s", ExecQueryString(filter_specs, constant, distinct_specs, sort_specs,
                            limit_spec, info->colUsed)
                .c_str());
  return SQLITE_OK;
}

int DataframeModule::Open(sqlite3_vtab*, sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int DataframeModule::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int DataframeModule::Filter(sqlite3_vtab_cursor* cur,
                            int,
                            const char* idx_str,
                            int argc,
                            sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  if (PERFETTO_UNLIKELY(idx_str != c->last_idx_str)) {
    auto* v = GetVtab(c->pVtab);
    auto* s = sqlite::ModuleStateManager<DataframeModule>::GetState(v->state);
    if (!s->dataframe->finalized()) {
      // A plan reads the table as it is when the plan is built, so one for a
      // table still being added to is built afresh for every run.
      auto built = LoadExecPlan(*s->dataframe, v->name, v->id_col_idx, idx_str);
      if (!built.ok()) {
        return sqlite::utils::SetError(c->pVtab, built.status());
      }
      c->exec.emplace(std::move(*built));
      c->last_idx_str = nullptr;
    } else {
      std::shared_ptr<const ExecPlan>& plan = v->exec_plans[idx_str];
      if (!plan) {
        auto built =
            LoadExecPlan(*s->dataframe, v->name, v->id_col_idx, idx_str);
        if (!built.ok()) {
          v->exec_plans.erase(idx_str);
          return sqlite::utils::SetError(c->pVtab, built.status());
        }
        plan = std::move(*built);
      }
      c->exec.emplace(plan);
      c->last_idx_str = idx_str;
    }
  }
  ExecQuery& q = *c->exec;
  SqliteValueFetcher fetcher(argv, argc);
  if (PERFETTO_UNLIKELY(!q.literals_set)) {
    // SQLite gives a constant the same value on every call of the statement.
    q.params.SetLiterals(fetcher);
    q.literals_set = true;
  }
  bool row = q.rows.Open(q.params.Bind(fetcher));
  c->eof = !row;
  if (PERFETTO_UNLIKELY(!row)) {
    return CheckExecStatus(c);
  }
  if (PERFETTO_UNLIKELY(!q.resolved)) {
    ResolveResults(&q);
  } else {
    CheckResults(&q);
  }
  // A lookup's row is rarely in cache, so start loading every cell SQLite will
  // read before it reads the first.
  for (const ExecQuery::Prefetch& p : q.prefetches) {
    q.rows.batch().column(p.index).Prefetch(q.rows.batch_row(), p.shift);
  }
  return SQLITE_OK;
}

void DataframeModule::ResolveResults(ExecQuery* q) {
  const core::exec::RowBatch& batch = q->rows.batch();
  q->prefetches.clear();
  for (uint32_t i = 0; i < q->plan->columns.size(); ++i) {
    uint32_t index = q->plan->columns[i];
    if (index == ExecPlan::kUnread) {
      q->results[i] = ExecQuery::Result{&ResultUnread, q->plan->id_index};
      continue;
    }
    const core::exec::ColumnView& view = batch.column(index);
    q->results[i] =
        ExecQuery::Result{ColumnResultFor(view), index, view.kind(),
                          static_cast<uint32_t>(view.type().index())};
    using Kind = core::exec::ColumnView::Kind;
    if (view.kind() == Kind::kFlat) {
      core::StorageType type = view.type();
      bool wide = type.Is<core::Int64>() || type.Is<core::Double>();
      q->prefetches.push_back({index, wide ? 3u : 2u});
    }
  }
  q->resolved = true;
}

void DataframeModule::CheckResults(ExecQuery* q) {
  const core::exec::RowBatch& batch = q->rows.batch();
  for (uint32_t i = 0; i < q->plan->columns.size(); ++i) {
    if (q->plan->columns[i] == ExecPlan::kUnread) {
      continue;
    }
    const ExecQuery::Result& result = q->results[i];
    const core::exec::ColumnView& view = batch.column(result.index);
    if (PERFETTO_UNLIKELY(view.kind() != result.kind ||
                          view.type().index() != result.type)) {
      ResolveResults(q);
      return;
    }
  }
}

int DataframeModule::CheckExecStatus(Cursor* c) {
  base::Status status = c->exec->rows.status();
  return status.ok() ? SQLITE_OK : sqlite::utils::SetError(c->pVtab, status);
}

int DataframeModule::Next(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  if (PERFETTO_LIKELY(c->exec->rows.Next())) {
    if (PERFETTO_UNLIKELY(c->exec->rows.first_in_batch())) {
      CheckResults(&*c->exec);
    }
    return SQLITE_OK;
  }
  c->eof = true;
  return CheckExecStatus(c);
}

int DataframeModule::Eof(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  return c->eof;
}

int DataframeModule::Column(sqlite3_vtab_cursor* cur,
                            sqlite3_context* ctx,
                            int raw_n) {
  auto* c = GetCursor(cur);
  const ExecQuery& q = *c->exec;
  const ExecQuery::Result& result = q.results[static_cast<uint32_t>(raw_n)];
  result.fn(ctx, q.plan->pool, q.rows.batch().column(result.index),
            q.rows.batch_row());
  return SQLITE_OK;
}

int DataframeModule::Rowid(sqlite3_vtab_cursor* cur, sqlite_int64* rowid) {
  auto* c = GetCursor(cur);
  *rowid = c->exec->rows.Value<uint32_t>(c->exec->plan->id_index);
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
