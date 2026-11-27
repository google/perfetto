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
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/logical_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/iterator.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"
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

bool IsColumnUsed(uint64_t cols_used, uint32_t col) {
  return (cols_used >> std::min(col, 63u)) & 1u;
}

pipeline::PreparedStatement BuildDataframeStatement(
    const dataframe::Dataframe& df,
    uint32_t id_column,
    const std::vector<dataframe::FilterSpec>& filters,
    const std::vector<bool>& constant,
    const std::vector<dataframe::DistinctSpec>& distinct,
    const std::vector<dataframe::SortSpec>& sorts,
    const dataframe::LimitSpec& limit,
    uint64_t cols_used) {
  uint32_t count = df.column_count();
  std::vector<bool> read(count, false);
  for (uint32_t i = 0; i < count; ++i) {
    read[i] = IsColumnUsed(cols_used, i);
  }
  read[id_column] = true;
  for (const dataframe::FilterSpec& f : filters) {
    if (f.op.IsAnyOf<core::NullOp>() || f.value_index) {
      read[f.col] = true;
    }
  }
  for (const dataframe::DistinctSpec& d : distinct) {
    read[d.col] = true;
  }
  for (const dataframe::SortSpec& s : sorts) {
    read[s.col] = true;
  }

  std::vector<uint32_t> scan_cols;
  scan_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (read[i]) {
      scan_cols.push_back(i);
    }
  }

  pipeline::Builder b;
  auto [rel, scanned] = b.Scan(df, scan_cols);
  std::vector<pipeline::ColumnId> col(count, 0);
  for (size_t i = 0; i < scan_cols.size(); ++i) {
    col[scan_cols[i]] = scanned[i].id;
  }

  std::vector<pipeline::Builder::WhereCondition> conditions;
  conditions.reserve(filters.size());
  for (const dataframe::FilterSpec& f : filters) {
    bool null_op = f.op.IsAnyOf<core::NullOp>();
    if (!null_op && !f.value_index) {
      continue;
    }
    pipeline::Builder::WhereCondition cond;
    cond.column = col[f.col];
    cond.op = f.op;
    if (!null_op) {
      cond.values.push_back(constant[f.source_index]
                                ? pipeline::Literal::External(*f.value_index)
                                : pipeline::Literal::Bound(*f.value_index));
    }
    conditions.push_back(std::move(cond));
  }
  if (!conditions.empty()) {
    rel = b.Where(rel, std::move(conditions));
  }

  if (!distinct.empty()) {
    std::vector<pipeline::ColumnId> keys;
    keys.reserve(distinct.size());
    for (const dataframe::DistinctSpec& d : distinct) {
      keys.push_back(col[d.col]);
    }
    rel = b.Distinct(rel, keys);
  }

  if (!sorts.empty()) {
    std::vector<pipeline::SortKey> order_keys;
    order_keys.reserve(sorts.size());
    for (const dataframe::SortSpec& s : sorts) {
      order_keys.push_back(
          {col[s.col], s.direction == dataframe::SortDirection::kDescending});
    }
    rel = b.OrderBy(rel, std::move(order_keys));
  }

  int64_t limit_val = limit.limit ? static_cast<int64_t>(*limit.limit) : -1;
  int64_t offset_val = limit.offset ? static_cast<int64_t>(*limit.offset) : 0;
  if (limit_val >= 0 || offset_val > 0) {
    rel = b.Limit(rel, limit_val, offset_val);
  }

  std::vector<pipeline::ColumnId> outputs;
  outputs.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (IsColumnUsed(cols_used, i) || i == id_column) {
      outputs.push_back(col[i]);
    }
  }
  return b.Finish(rel, outputs);
}

PERFETTO_NO_INLINE int PrepareFilter(DataframeModule::Cursor* c,
                                     const char* idx_str) {
  auto* v = DataframeModule::GetVtab(c->pVtab);
  auto* s = sqlite::ModuleStateManager<DataframeModule>::GetState(v->state);
  std::optional<std::string> bytes = base::Base64Decode(idx_str);
  PERFETTO_CHECK(bytes);
  pipeline::PreparedStatement stmt =
      pipeline::PreparedStatement::Deserialize(*bytes);
  const std::vector<std::string>& col_names = s->dataframe->column_names();
  std::vector<uint32_t> col_to_output(col_names.size(),
                                      DataframeModule::ExecQuery::kUnread);
  // Outputs are emitted in ascending dataframe column index order.
  uint32_t df_col = 0;
  for (uint32_t out = 0; out < stmt.column_count(); ++out) {
    const std::string& name = stmt.column_name(out);
    while (col_names[df_col] != name) {
      ++df_col;
      PERFETTO_DCHECK(df_col < col_names.size());
    }
    col_to_output[df_col++] = out;
  }
  const dataframe::Dataframe* df_ptr = s->dataframe;
  pipeline::ExecutionContext ctx{
      s->dataframe->string_pool(), nullptr, nullptr, {&df_ptr, &df_ptr + 1}};
  auto rows = std::move(stmt).ExecuteRows(ctx);
  if (!rows.ok()) {
    return sqlite::utils::SetError(c->pVtab, rows.status());
  }
  uint32_t id_output_col = col_to_output[v->id_col_idx];
  c->exec.emplace(std::move(*rows), std::move(col_to_output), id_output_col);
  // Rebuild per Filter while rows are still being added to the table.
  c->last_idx_str = s->dataframe->finalized() ? idx_str : nullptr;
  return SQLITE_OK;
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
        // The contract lets us dedup over the colUsed columns. SQLite folds
        // columns >= 63 into bit 63, so only push down DISTINCT when colUsed
        // identifies the exact columns.
        uint32_t col_count = s->dataframe->column_count();
        if (col_count <= 64 || ((info->colUsed >> 63) & 1u) == 0) {
          for (uint32_t i = 0; i < std::min(col_count, 64u); ++i) {
            if ((info->colUsed >> i) & 1u) {
              distinct_specs.push_back(dataframe::DistinctSpec{i});
            }
          }
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
      s->dataframe->PlanQueryLogical(filter_specs, distinct_specs, sort_specs,
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
  info->estimatedCost = plan.EstimatedCost();
  info->estimatedRows = plan.rows.estimated;
  if (plan.rows.max <= 1) {
    info->idxFlags |= SQLITE_INDEX_SCAN_UNIQUE;
  }
  info->idxNum = v->best_idx_num++;
  PERFETTO_TP_TRACE(
      metatrace::Category::QUERY_TIMELINE, "DATAFRAME_BEST_INDEX",
      [info, v, s](metatrace::Record* record) {
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
  pipeline::PreparedStatement stmt = BuildDataframeStatement(
      *s->dataframe, v->id_col_idx, filter_specs, constant, distinct_specs,
      sort_specs, limit_spec, info->colUsed);
  std::string serialized = stmt.Serialize();
  std::string encoded =
      base::Base64Encode(serialized.data(), serialized.size());
  info->idxStr = sqlite3_mprintf("%s", encoded.c_str());
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
                            int,
                            sqlite3_value** argv) {
  auto* c = GetCursor(cur);
  if (PERFETTO_UNLIKELY(idx_str != c->last_idx_str)) {
    int rc = PrepareFilter(c, idx_str);
    if (rc != SQLITE_OK) {
      return rc;
    }
  }
  ExecQuery& q = *c->exec;
  SqliteValueFetcher fetcher(argv);
  if (PERFETTO_UNLIKELY(!q.literals_set)) {
    // SQLite gives a constant the same value on every call of the statement.
    q.params.SetExternalLiterals(fetcher);
    q.literals_set = true;
  }
  bool row = q.rows.Open(q.params.Bind(fetcher));
  c->eof = !row;
  if (PERFETTO_UNLIKELY(!row)) {
    const base::Status& status = q.rows.status();
    return PERFETTO_LIKELY(status.ok())
               ? SQLITE_OK
               : sqlite::utils::SetError(c->pVtab, status);
  }
  return SQLITE_OK;
}

int DataframeModule::Next(sqlite3_vtab_cursor* cur) {
  auto* c = GetCursor(cur);
  if (PERFETTO_LIKELY(c->exec->rows.Next())) {
    return SQLITE_OK;
  }
  c->eof = true;
  const base::Status& status = c->exec->rows.status();
  if (PERFETTO_LIKELY(status.ok())) {
    return SQLITE_OK;
  }
  return sqlite::utils::SetError(c->pVtab, status);
}

int DataframeModule::Eof(sqlite3_vtab_cursor* cur) {
  return GetCursor(cur)->eof;
}

int DataframeModule::Column(sqlite3_vtab_cursor* cur,
                            sqlite3_context* ctx,
                            int raw_n) {
  auto* c = GetCursor(cur);
  const ExecQuery& q = *c->exec;
  uint32_t out_col = q.col_to_output[static_cast<uint32_t>(raw_n)];
  if (PERFETTO_UNLIKELY(out_col == ExecQuery::kUnread)) {
    sqlite::result::Null(ctx);
    return SQLITE_OK;
  }
  q.rows.Cell(out_col, SqliteResultCallback{{}, ctx});
  return SQLITE_OK;
}

struct RowidCallback {
  void OnCell(int64_t v) const { *rowid = v; }
  PERFETTO_NORETURN void OnCell(double) const {
    PERFETTO_FATAL("Unexpected type for rowid");
  }
  PERFETTO_NORETURN void OnCell(NullTermStringView) const {
    PERFETTO_FATAL("Unexpected type for rowid");
  }
  PERFETTO_NORETURN void OnCell(std::nullptr_t) const {
    PERFETTO_FATAL("Unexpected type for rowid");
  }
  void OnCell(uint32_t v) const { *rowid = static_cast<sqlite_int64>(v); }
  void OnCell(int32_t v) const { *rowid = static_cast<sqlite_int64>(v); }
  sqlite_int64* rowid;
};

int DataframeModule::Rowid(sqlite3_vtab_cursor* cur, sqlite_int64* rowid) {
  auto* c = GetCursor(cur);
  c->exec->rows.Cell(c->exec->id_output_col, RowidCallback{rowid});
  return SQLITE_OK;
}

}  // namespace perfetto::trace_processor
