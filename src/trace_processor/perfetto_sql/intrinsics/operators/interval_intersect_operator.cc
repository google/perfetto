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

#include "src/trace_processor/perfetto_sql/intrinsics/operators/interval_intersect_operator.h"

#include <sqlite3.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/module_lifecycle_manager.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

using Op = IntervalIntersectOperator;
using Cursor = Op::Cursor;
using Manager = sqlite::ModuleStateManager<Op>;
using ColumnsMap = Op::SchemaToTableColumnMap;

constexpr char kSliceSchema[] = R"(
  CREATE TABLE x(
    tab TEXT HIDDEN,
    exposed_cols_str TEXT HIDDEN,
    ts BIGINT,
    ts_end BIGINT,
    id BIGINT,
    c0 ANY,
    c1 ANY,
    c2 ANY,
    c3 ANY,
    c4 ANY,
    c5 ANY,
    c6 ANY,
    c7 ANY,
    c8 ANY,
    PRIMARY KEY(id)
  ) WITHOUT ROWID
)";

enum SchemaColumnIds {
  kTableName = 0,
  kExposedCols = 1,
  kTs = 2,
  kTsEnd = 3,
  kId = 4,
  kAdditional = 5,
  kMaxCol = 13
};

constexpr uint32_t kArgsCount = 2;

inline void HashSqlValue(base::Hasher& h, const SqlValue& v) {
  switch (v.type) {
    case SqlValue::Type::kString:
      h.Update(v.AsString());
      break;
    case SqlValue::Type::kDouble:
      h.Update(v.AsDouble());
      break;
    case SqlValue::Type::kLong:
      h.Update(v.AsLong());
      break;
    case SqlValue::Type::kBytes:
      PERFETTO_FATAL("Wrong type");
      break;
    case SqlValue::Type::kNull:
      h.Update(nullptr);
      break;
  }
  return;
}

base::StatusOr<uint32_t> ColIdForName(const Table* t,
                                      const std::string& col_name,
                                      const std::string& table_name) {
  auto x = t->ColumnIdxFromName(col_name);
  if (!x.has_value()) {
    return base::ErrStatus("interval_intersect: No column '%s' in table '%s'",
                           col_name.c_str(), table_name.c_str());
  }
  return *x;
}

base::StatusOr<Cursor::TreesMap> CreateIntervalTrees(
    const Table* t,
    const std::string& table_name,
    const ColumnsMap& cols) {
  uint32_t ts_col_idx = 0;
  ASSIGN_OR_RETURN(ts_col_idx, ColIdForName(t, "ts", table_name));
  uint32_t ts_end_col_idx = 0;
  ASSIGN_OR_RETURN(ts_end_col_idx, ColIdForName(t, "ts_end", table_name));
  uint32_t id_col_idx = 0;
  ASSIGN_OR_RETURN(id_col_idx, ColIdForName(t, "id", table_name));

  std::vector<Op::SchemaCol> cols_for_tree;
  for (const auto& c : cols) {
    if (c) {
      cols_for_tree.push_back(*c);
    }
  }

  base::FlatHashMap<Cursor::TreesKey, std::vector<IntervalTree::Interval>>
      sorted_intervals;
  for (Table::Iterator it = t->IterateRows(); it; ++it) {
    IntervalTree::Interval i;
    i.start = static_cast<uint64_t>(it.Get(ts_col_idx).AsLong());
    i.end = static_cast<uint64_t>(it.Get(ts_end_col_idx).AsLong());
    i.id = static_cast<uint32_t>(it.Get(id_col_idx).AsLong());

    base::Hasher h;
    for (const auto& c : cols_for_tree) {
      SqlValue v = it.Get(c);
      HashSqlValue(h, v);
    }
    sorted_intervals[h.digest()].push_back(i);
  }

  Cursor::TreesMap ret;
  for (auto it = sorted_intervals.GetIterator(); it; ++it) {
    IntervalTree x(it.value());
    ret[it.key()] = std::make_unique<IntervalTree>(std::move(x));
  }
  return std::move(ret);
}

base::StatusOr<SqlValue> GetRhsValue(sqlite3_index_info* info,
                                     SchemaColumnIds col) {
  sqlite3_value* val = nullptr;

  int ret = -1;
  for (int i = 0; i < info->nConstraint; ++i) {
    auto c = info->aConstraint[i];
    if (sqlite::utils::IsOpEq(c.op) && c.iColumn == col)
      ret = sqlite3_vtab_rhs_value(info, i, &val);
  }
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Invalid RHS value.");
  }

  return sqlite::utils::SqliteValueToSqlValue(val);
}

base::StatusOr<const Table*> GetTableFromRhsValue(PerfettoSqlEngine* engine,
                                                  sqlite3_index_info* info) {
  ASSIGN_OR_RETURN(SqlValue table_name_val, GetRhsValue(info, kTableName));
  if (table_name_val.type != SqlValue::kString) {
    return base::ErrStatus("Table name is not a string");
  }

  const std::string table_name = table_name_val.AsString();
  const Table* t = engine->GetTableOrNull(table_name);
  if (!t) {
    return base::ErrStatus("Table not registered");
  }
  return t;
}

base::StatusOr<ColumnsMap> GetExposedColumns(
    const std::string& exposed_cols_str,
    const Table* tab) {
  ColumnsMap ret;
  for (const std::string& col : base::SplitString(exposed_cols_str, ",")) {
    std::string col_name = base::TrimWhitespace(col);
    auto table_i = tab->ColumnIdxFromName(col_name);
    if (!table_i) {
      return base::ErrStatus("Didn't find column '%s'", col_name.c_str());
    }
    uint32_t schema_idx =
        *base::CStringToUInt32(
            std::string(col_name.begin() + 1, col_name.end()).c_str()) +
        kAdditional;
    ret[schema_idx] = static_cast<uint16_t>(*table_i);
  }
  return ret;
}

base::Status CreateCursorInnerData(Cursor::InnerData* inner,
                                   PerfettoSqlEngine* engine,
                                   const std::string& table_name,
                                   const ColumnsMap& cols) {
  // Build the tree for the runtime table if possible
  const Table* t = engine->GetTableOrNull(table_name);
  if (!t) {
    return base::ErrStatus("interval_intersect operator: table not found");
  }
  ASSIGN_OR_RETURN(inner->trees, CreateIntervalTrees(t, table_name, cols));
  return base::OkStatus();
}

base::Status CreateCursorOuterData(const Table* t,
                                   Cursor::OuterData* outer,
                                   const std::string& table_name) {
  outer->it = std::make_unique<Table::Iterator>(t->IterateRows());

  ASSIGN_OR_RETURN(outer->additional_cols[kId],
                   ColIdForName(t, "id", table_name));
  ASSIGN_OR_RETURN(outer->additional_cols[kTs],
                   ColIdForName(t, "ts", table_name));
  ASSIGN_OR_RETURN(outer->additional_cols[kTsEnd],
                   ColIdForName(t, "ts_end", table_name));

  return base::OkStatus();
}

}  // namespace

int IntervalIntersectOperator::Connect(sqlite3* db,
                                       void* raw_ctx,
                                       int,
                                       const char* const* argv,
                                       sqlite3_vtab** vtab,
                                       char**) {
  // No args because we are not creating vtab, not like mipmap op.
  if (int ret = sqlite3_declare_vtab(db, kSliceSchema); ret != SQLITE_OK) {
    return ret;
  }

  // Create the state to access the engine in Filter.
  auto ctx = GetContext(raw_ctx);
  auto state = std::make_unique<State>();
  state->engine = ctx->engine;

  std::unique_ptr<Vtab> res = std::make_unique<Vtab>();
  res->state = ctx->manager.OnCreate(argv, std::move(state));
  *vtab = res.release();
  return SQLITE_OK;
}

int IntervalIntersectOperator::Disconnect(sqlite3_vtab* vtab) {
  std::unique_ptr<Vtab> tab(GetVtab(vtab));
  sqlite::ModuleStateManager<IntervalIntersectOperator>::OnDestroy(tab->state);
  return SQLITE_OK;
}

int IntervalIntersectOperator::BestIndex(sqlite3_vtab* t,
                                         sqlite3_index_info* info) {
  int n = info->nConstraint;

  // Validate `table_name` constraint. We expect it to be a constraint on
  // equality and on the kTableName column.
  base::Status args_status =
      sqlite::utils::ValidateFunctionArguments(info, kArgsCount, [](int c) {
        return c == SchemaColumnIds::kTableName ||
               c == SchemaColumnIds::kExposedCols;
      });

  PERFETTO_CHECK(args_status.ok());
  if (!args_status.ok()) {
    return SQLITE_CONSTRAINT;
  }

  // Find real rows count
  PerfettoSqlEngine* engine = Manager::GetState(GetVtab(t)->state)->engine;
  SQLITE_ASSIGN_OR_RETURN(t, const Table* tab,
                          GetTableFromRhsValue(engine, info));
  if (!t) {
    return sqlite::utils::SetError(t, "Table not registered");
  }
  uint32_t rows_count = tab->row_count();
  info->estimatedRows = rows_count;

  // Count usable constraints among args and required schema.
  uint32_t count_usable = 0;
  for (int i = 0; i < n; ++i) {
    auto c = info->aConstraint[i];
    if (c.iColumn < kAdditional) {
      count_usable += c.usable;
    }
  }

  // There is nothing more to do for only args constraints, which happens for
  // the kOuter operator.
  if (count_usable == kArgsCount) {
    info->idxNum = kOuter;
    info->estimatedCost = rows_count;
    return SQLITE_OK;
  }

  // For inner we expect all constraints to be usable.
  PERFETTO_CHECK(count_usable == 4);
  if (count_usable != 4) {
    return SQLITE_CONSTRAINT;
  }

  info->idxNum = kInner;

  // Cost of querying centered interval tree.
  info->estimatedCost = log2(rows_count);

  // We are now doing BestIndex of kInner.

  auto ts_found = false;
  auto ts_end_found = false;
  int argv_index = kAdditional;
  auto* s = Manager::GetState(GetVtab(t)->state);

  for (int i = 0; i < n; ++i) {
    const auto& c = info->aConstraint[i];

    // Ignore table_name constraints as we validated it before.
    if (c.iColumn == kTableName || c.iColumn == kExposedCols) {
      continue;
    }

    // We should omit all constraints.
    // TODO(mayzner): Remove after we support handling other columns.
    auto& usage = info->aConstraintUsage[i];
    usage.omit = true;

    // The constraints we are looking for is `A.ts < B.ts_end AND A.ts_end >
    // B.ts`. That is why for `ts` column we can only have `kLt` operator and
    // for `ts_end` only `kGt`.

    // Add `ts` constraint.
    if (c.iColumn == kTs && !ts_found) {
      ts_found = true;
      if (!sqlite::utils::IsOpLt(c.op)) {
        return sqlite::utils::SetError(
            t, "interval_intersect operator: `ts` columns has wrong operation");
      }
      // The index is moved by one.
      usage.argvIndex = kTs + 1;
      continue;
    }

    // Add `ts_end` constraint.
    if (c.iColumn == kTsEnd && !ts_end_found) {
      ts_end_found = true;
      if (!sqlite::utils::IsOpGt(c.op)) {
        return sqlite::utils::SetError(t,
                                       "interval_intersect operator: `ts_end` "
                                       "columns has wrong operation");
      }
      usage.argvIndex = kTsEnd + 1;
      continue;
    }

    if (c.iColumn >= kAdditional) {
      if (!sqlite::utils::IsOpEq(c.op)) {
        return sqlite::utils::SetError(t,
                                       "interval_intersect operator: `ts_end` "
                                       "columns has wrong operation");
      }
      usage.argvIndex = argv_index++;
      s->argv_to_col_map[static_cast<size_t>(c.iColumn)] = usage.argvIndex;
      continue;
    }

    return sqlite::utils::SetError(
        t, "interval_intersect operator: wrong constraint");
  }

  return SQLITE_OK;
}

int IntervalIntersectOperator::Open(sqlite3_vtab*,
                                    sqlite3_vtab_cursor** cursor) {
  std::unique_ptr<Cursor> c = std::make_unique<Cursor>();
  *cursor = c.release();
  return SQLITE_OK;
}

int IntervalIntersectOperator::Close(sqlite3_vtab_cursor* cursor) {
  std::unique_ptr<Cursor> c(GetCursor(cursor));
  return SQLITE_OK;
}

int IntervalIntersectOperator::Filter(sqlite3_vtab_cursor* cursor,
                                      int idxNum,
                                      const char*,
                                      int,
                                      sqlite3_value** argv) {
  auto* c = GetCursor(cursor);
  c->type = static_cast<OperatorType>(idxNum);

  auto* t = GetVtab(c->pVtab);
  PerfettoSqlEngine* engine = Manager::GetState(t->state)->engine;

  // Table name constraint.
  auto table_name_sql_val = sqlite::utils::SqliteValueToSqlValue(argv[0]);
  if (table_name_sql_val.type != SqlValue::kString) {
    return sqlite::utils::SetError(
        t, "interval_intersect operator: table name is not a string");
  }
  std::string table_name = table_name_sql_val.AsString();

  // Exposed columns constraint.
  auto exposed_cols_sql_val = sqlite::utils::SqliteValueToSqlValue(argv[1]);
  if (exposed_cols_sql_val.type != SqlValue::kString) {
    return sqlite::utils::SetError(
        t, "interval_intersect operator: exposed columns is not a string");
  }
  std::string exposed_cols_str = exposed_cols_sql_val.AsString();

  // If the cursor has different table cached or differenct cols reset the
  // cursor.
  if (c->table_name != table_name || exposed_cols_str != c->exposed_cols_str) {
    c->inner.trees.Clear();
    c->outer.it.reset();
  }
  c->exposed_cols_str = exposed_cols_str;

  if (c->type == kOuter) {
    // We expect this function to be called only once per table, so recreate
    // this if needed.
    c->table = engine->GetTableOrNull(table_name);
    c->table_name = table_name;
    SQLITE_ASSIGN_OR_RETURN(t, c->outer.additional_cols,
                            GetExposedColumns(c->exposed_cols_str, c->table));
    SQLITE_RETURN_IF_ERROR(
        t, CreateCursorOuterData(c->table, &c->outer, table_name));
    return SQLITE_OK;
  }

  PERFETTO_DCHECK(c->type == kInner);
  const auto argv_map = Manager::GetState(GetVtab(t)->state)->argv_to_col_map;

  // Create inner cursor if tree doesn't exist.
  if (c->inner.trees.size() == 0) {
    c->table = engine->GetTableOrNull(table_name);
    c->table_name = table_name;
    Op::SchemaToTableColumnMap exposed_cols_map;
    SQLITE_ASSIGN_OR_RETURN(t, exposed_cols_map,
                            GetExposedColumns(c->exposed_cols_str, c->table));
    SchemaToTableColumnMap new_map;
    for (uint32_t i = 0; i < Op::kSchemaColumnsCount; i++) {
      if (argv_map[i]) {
        new_map[i] = exposed_cols_map[i];
      }
    }

    SQLITE_RETURN_IF_ERROR(
        c->pVtab,
        CreateCursorInnerData(&c->inner, engine, table_name, new_map));
  }

  // Query |c.tree| on the interval and materialize the results.
  auto ts_constraint = sqlite::utils::SqliteValueToSqlValue(argv[kTs]);
  if (ts_constraint.type != SqlValue::kLong) {
    return sqlite::utils::SetError(
        t, "interval_intersect operator: `ts` constraint has to be a number");
  }

  auto ts_end_constraint = sqlite::utils::SqliteValueToSqlValue(argv[kTsEnd]);
  if (ts_end_constraint.type != SqlValue::kLong) {
    return sqlite::utils::SetError(
        t,
        "interval_intersect operator: `ts_end` constraint has to be a number");
  }

  uint64_t end = static_cast<uint64_t>(ts_constraint.AsLong());
  uint64_t start = static_cast<uint64_t>(ts_end_constraint.AsLong());

  base::Hasher h;
  for (uint32_t i = 0; i < argv_map.size(); i++) {
    if (argv_map[i]) {
      uint32_t x = *argv_map[i];
      HashSqlValue(h, sqlite::utils::SqliteValueToSqlValue(argv[x - 1]));
    }
  }

  c->inner.Query(start, end, h.digest());

  return SQLITE_OK;
}

int IntervalIntersectOperator::Next(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);

  switch (c->type) {
    case kInner:
      c->inner.index++;
      break;
    case kOuter:
      ++(*c->outer.it);
      break;
  }

  return SQLITE_OK;
}

int IntervalIntersectOperator::Eof(sqlite3_vtab_cursor* cursor) {
  auto* c = GetCursor(cursor);

  switch (c->type) {
    case kInner:
      return c->inner.index >= c->inner.query_results.size();
    case kOuter:
      return !(*c->outer.it);
  }
  PERFETTO_FATAL("For GCC");
}

int IntervalIntersectOperator::Column(sqlite3_vtab_cursor* cursor,
                                      sqlite3_context* ctx,
                                      int N) {
  auto* c = GetCursor(cursor);

  if (c->type == kInner) {
    PERFETTO_DCHECK(N == kId);
    sqlite::result::Long(ctx, c->inner.GetResultId());
    return SQLITE_OK;
  }

  PERFETTO_CHECK(c->type == kOuter);

  switch (N) {
    case kTs:
      sqlite::result::Long(ctx, c->outer.Get(kTs).AsLong());
      break;
    case kTsEnd:
      sqlite::result::Long(ctx, c->outer.Get(kTsEnd).AsLong());
      break;
    case kId:
      sqlite::result::Long(ctx, c->outer.Get(kId).AsLong());
      break;
    case kExposedCols:
    case kTableName:
      return sqlite::utils::SetError(
          GetVtab(cursor->pVtab),
          "interval_intersect operator: invalid column");
    default:
      PERFETTO_DCHECK(N >= kAdditional && N <= kMaxCol);
      sqlite::utils::ReportSqlValue(ctx, c->outer.Get(N));
      break;
  }

  return SQLITE_OK;
}

int IntervalIntersectOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor
