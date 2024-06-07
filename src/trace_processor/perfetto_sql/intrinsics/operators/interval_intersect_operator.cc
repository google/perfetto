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
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/module_lifecycle_manager.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

using Cursor = IntervalIntersectOperator::Cursor;
using Manager = sqlite::ModuleStateManager<IntervalIntersectOperator>;

constexpr char kSliceSchema[] = R"(
  CREATE TABLE x(
    tab TEXT HIDDEN,
    ts BIGINT,
    ts_end BIGINT,
    id BIGINT,
    PRIMARY KEY(id)
  ) WITHOUT ROWID
)";

enum SchemaColumns { kTableName = 0, kTs = 1, kTsEnd = 2, kId = 3 };

base::StatusOr<uint32_t> ColIdForName(const Table::Iterator& it,
                                      std::string col_name,
                                      std::string table_name) {
  auto x = it.ColumnIdxFromName(col_name);
  if (!x.has_value()) {
    return base::ErrStatus("interval_intersect: No column '%s' in table '%s'",
                           col_name.c_str(), table_name.c_str());
  }
  return *x;
}

base::StatusOr<IntervalTree> CreateIntervalTree(Table::Iterator it,
                                                std::string table_name) {
  uint32_t ts_col_idx = 0;
  ASSIGN_OR_RETURN(ts_col_idx, ColIdForName(it, "ts", table_name));
  uint32_t ts_end_col_idx = 0;
  ASSIGN_OR_RETURN(ts_end_col_idx, ColIdForName(it, "ts_end", table_name));
  uint32_t id_col_idx = 0;
  ASSIGN_OR_RETURN(id_col_idx, ColIdForName(it, "id", table_name));

  std::vector<IntervalTree::Interval> sorted_intervals;
  for (; it; ++it) {
    IntervalTree::Interval i;
    i.start = static_cast<uint64_t>(it.Get(ts_col_idx).AsLong());
    i.end = static_cast<uint64_t>(it.Get(ts_end_col_idx).AsLong());
    i.id = static_cast<uint32_t>(it.Get(id_col_idx).AsLong());
    sorted_intervals.push_back(i);
  }

  return IntervalTree(sorted_intervals);
}

// Can only be called in BestIndex
base::Status RowsCount(PerfettoSqlEngine* engine,
                       sqlite3_index_info* info,
                       uint32_t* rows_count) {
  sqlite3_value* val = nullptr;

  int ret = -1;
  for (int i = 0; i < info->nConstraint; ++i) {
    if (sqlite::utils::IsOpEq(info->aConstraint[i].op))
      ret = sqlite3_vtab_rhs_value(info, i, &val);
  }
  if (ret != SQLITE_OK) {
    return base::ErrStatus("Invalid RHS value.");
  }

  SqlValue table_name_val = sqlite::utils::SqliteValueToSqlValue(val);
  if (table_name_val.type != SqlValue::kString) {
    return base::ErrStatus("Table name is not a string");
  }

  std::string table_name = table_name_val.AsString();
  const Table* t = engine->GetTableOrNull(table_name);
  if (!t) {
    return base::ErrStatus("Table not registered");
  }
  *rows_count = t->row_count();
  return base::OkStatus();
}

base::Status CreateCursorInnerData(Cursor::InnerData* inner,
                                   PerfettoSqlEngine* engine,
                                   std::string table_name) {
  // Build the tree for the runtime table if possible
  const Table* t = engine->GetTableOrNull(table_name);
  if (!t) {
    return base::ErrStatus("interval_intersect operator: table not found");
  }
  ASSIGN_OR_RETURN(IntervalTree tree,
                   CreateIntervalTree(t->IterateRows(), table_name));
  inner->tree = std::make_unique<IntervalTree>(std::move(tree));
  return base::OkStatus();
}

base::Status CreateCursorOuterData(Cursor::OuterData* outer,
                                   PerfettoSqlEngine* engine,
                                   std::string table_name) {
  const Table* t = engine->GetTableOrNull(table_name);
  if (!t) {
    return base::ErrStatus("interval_intersect operator: table not found");
  }
  outer->it = std::make_unique<Table::Iterator>(t->IterateRows());

  ASSIGN_OR_RETURN(outer->id_col_id,
                   ColIdForName(*outer->it, "id", table_name));
  ASSIGN_OR_RETURN(outer->ts_col_id,
                   ColIdForName(*outer->it, "ts", table_name));
  ASSIGN_OR_RETURN(outer->ts_end_col_id,
                   ColIdForName(*outer->it, "ts_end", table_name));

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
  // We expect 1 constraint for operators that we will be queried multiple times
  // - those should have interval trees build on them. 3 constraints mean that
  // we got the table name, `ts` and `ts_end`, so this is the table that should
  // query the interval tree.
  int n = info->nConstraint;

  if (n != 3) {
    return SQLITE_CONSTRAINT;
  }

  // Validate `table_name` constraint. We expect it to be a constraint on
  // equality and on the kTableName column.
  base::Status table_name_status = sqlite::utils::ValidateFunctionArguments(
      info, 1, [](int c) { return c == SchemaColumns::kTableName; });
  if (!table_name_status.ok()) {
    return SQLITE_CONSTRAINT;
  }

  uint32_t rows_num;
  if (auto status = RowsCount(Manager::GetState(GetVtab(t)->state)->engine,
                              info, &rows_num);
      !status.ok()) {
    return sqlite::utils::SetError(t, status.c_message());
  }
  info->estimatedRows = rows_num;

  // Both of operator types will have `nConstraint==3`, but the `kOuter` will
  // only have 1 usable constraint.

  uint32_t count_usable = 0;
  for (int i = 0; i < n; ++i) {
    count_usable += info->aConstraint[i].usable;
  }

  // There is nothing more to do for one constraint, which happens for the
  // kOuter operator.
  if (count_usable == 1) {
    info->idxNum = kOuter;
    info->estimatedCost = rows_num;
    return SQLITE_OK;
  }

  // For inner we expect all constraints to be usable.
  if (count_usable != 3) {
    return SQLITE_CONSTRAINT;
  }

  info->idxNum = kInner;

  // Cost of querying centered interval tree.
  info->estimatedCost = log2(rows_num);

  // We are now doing BestIndex of kInner.

  auto ts_found = false;
  auto ts_end_found = false;

  for (int i = 0; i < n; ++i) {
    const auto& c = info->aConstraint[i];

    // Ignore table_name constraints as we validated it before.
    if (c.iColumn == kTableName) {
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
      // The index is moved by one.
      usage.argvIndex = kTsEnd + 1;
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
                                      int argc,
                                      sqlite3_value** argv) {
  auto* c = GetCursor(cursor);
  c->type = static_cast<OperatorType>(idxNum);
  PERFETTO_DCHECK(argc == 1 || argc == 3);

  auto* t = GetVtab(c->pVtab);

  // Table name constraint.
  auto table_name = sqlite::utils::SqliteValueToSqlValue(argv[0]);
  if (table_name.type != SqlValue::kString) {
    return sqlite::utils::SetError(
        t, "interval_intersect operator: table name is not a string");
  }

  // If the cursor has different table cached reset the cursor.
  if (c->table_name != table_name.AsString()) {
    c->inner.tree.reset();
    c->outer.it.reset();
  }

  if (c->type == kOuter) {
    // We expect this function to be called only once per table, so recreate
    // this if needed.
    c->table_name = table_name.AsString();
    if (auto s = CreateCursorOuterData(&c->outer,
                                       Manager::GetState(t->state)->engine,
                                       table_name.AsString());
        !s.ok()) {
      return sqlite::utils::SetError(t, s);
    }
    return SQLITE_OK;
  }

  PERFETTO_DCHECK(c->type == kInner);

  // Create |c.tree| if it doesn't exist.
  if (c->inner.tree == nullptr) {
    c->table_name = table_name.AsString();
    if (auto s = CreateCursorInnerData(&c->inner,
                                       Manager::GetState(t->state)->engine,
                                       table_name.AsString());
        !s.ok()) {
      return sqlite::utils::SetError(t, s);
    }
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
  c->inner.Query(start, end);

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
      return c->inner.index >= c->inner.result_ids.size();
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
      sqlite::result::Long(ctx, c->outer.GetTs());
      break;
    case kTsEnd:
      sqlite::result::Long(ctx, c->outer.GetTsEnd());
      break;
    case kId:
      sqlite::result::Long(ctx, c->outer.GetId());
      break;
    default:
      return sqlite::utils::SetError(
          GetVtab(cursor->pVtab),
          "interval_intersect operator: invalid column");
  }

  return SQLITE_OK;
}

int IntervalIntersectOperator::Rowid(sqlite3_vtab_cursor*, sqlite_int64*) {
  return SQLITE_ERROR;
}

}  // namespace perfetto::trace_processor
