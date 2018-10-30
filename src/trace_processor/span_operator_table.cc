/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/span_operator_table.h"

#include <sqlite3.h>
#include <string.h>
#include <algorithm>
#include <set>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr uint64_t kU64Max = std::numeric_limits<uint64_t>::max();

std::vector<Table::Column> GetColumnsForTable(
    sqlite3* db,
    const std::string& raw_table_name) {
  char sql[1024];
  const char kRawSql[] = "SELECT name, type from pragma_table_info(\"%s\")";

  // Support names which are table valued functions with arguments.
  std::string table_name = raw_table_name.substr(0, raw_table_name.find('('));
  int n = snprintf(sql, sizeof(sql), kRawSql, table_name.c_str());
  PERFETTO_DCHECK(n >= 0 || static_cast<size_t>(n) < sizeof(sql));

  sqlite3_stmt* raw_stmt = nullptr;
  int err = sqlite3_prepare_v2(db, sql, n, &raw_stmt, nullptr);

  ScopedStmt stmt(raw_stmt);
  PERFETTO_DCHECK(sqlite3_column_count(*stmt) == 2);

  std::vector<Table::Column> columns;
  while (true) {
    err = sqlite3_step(raw_stmt);
    if (err == SQLITE_DONE)
      break;
    if (err != SQLITE_ROW) {
      PERFETTO_ELOG("Querying schema of table failed");
      return {};
    }

    const char* name =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 0));
    const char* raw_type =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 1));
    if (!name || !raw_type || !*name || !*raw_type) {
      PERFETTO_ELOG("Schema has invalid column values");
      return {};
    }

    Table::ColumnType type;
    if (strcmp(raw_type, "UNSIGNED BIG INT") == 0) {
      type = Table::ColumnType::kUlong;
    } else if (strcmp(raw_type, "UNSIGNED INT") == 0) {
      type = Table::ColumnType::kUint;
    } else if (strcmp(raw_type, "STRING") == 0) {
      type = Table::ColumnType::kString;
    } else {
      PERFETTO_FATAL("Unknown column type on table %s", raw_table_name.c_str());
    }
    columns.emplace_back(columns.size(), name, type);
  }
  return columns;
}

}  // namespace

SpanOperatorTable::SpanOperatorTable(sqlite3* db, const TraceStorage*)
    : db_(db) {}

void SpanOperatorTable::RegisterTable(sqlite3* db,
                                      const TraceStorage* storage) {
  Table::Register<SpanOperatorTable>(db, storage, "span");
}

Table::Schema SpanOperatorTable::CreateSchema(int argc,
                                              const char* const* argv) {
  // argv[0] - argv[2] are SQLite populated fields which are always present.
  if (argc < 6) {
    PERFETTO_ELOG("SPAN JOIN expected at least 3 args, received %d", argc - 3);
    return Table::Schema({}, {});
  }

  // The order arguments is (t1_name, t2_name, join_col).
  t1_defn_.name = reinterpret_cast<const char*>(argv[3]);
  t1_defn_.cols = GetColumnsForTable(db_, t1_defn_.name);

  t2_defn_.name = reinterpret_cast<const char*>(argv[4]);
  t2_defn_.cols = GetColumnsForTable(db_, t2_defn_.name);

  join_col_ = reinterpret_cast<const char*>(argv[5]);

  // TODO(lalitm): add logic to ensure that the tables that are being joined
  // are actually valid to be joined i.e. they have the ts and dur columns and
  // have the join column.

  auto filter_fn = [this](const Table::Column& it) {
    return it.name() == "ts" || it.name() == "dur" || it.name() == join_col_;
  };
  auto t1_remove_it =
      std::remove_if(t1_defn_.cols.begin(), t1_defn_.cols.end(), filter_fn);
  t1_defn_.cols.erase(t1_remove_it, t1_defn_.cols.end());
  auto t2_remove_it =
      std::remove_if(t2_defn_.cols.begin(), t2_defn_.cols.end(), filter_fn);
  t2_defn_.cols.erase(t2_remove_it, t2_defn_.cols.end());

  std::vector<Table::Column> columns = {
      Table::Column(Column::kTimestamp, "ts", ColumnType::kUlong),
      Table::Column(Column::kDuration, "dur", ColumnType::kUlong),
      Table::Column(Column::kJoinValue, join_col_, ColumnType::kUlong),
  };
  size_t index = kReservedColumns;
  for (const auto& col : t1_defn_.cols) {
    columns.emplace_back(index++, col.name(), col.type());
  }
  for (const auto& col : t2_defn_.cols) {
    columns.emplace_back(index++, col.name(), col.type());
  }
  return Schema(columns, {Column::kTimestamp, kJoinValue});
}

std::unique_ptr<Table::Cursor> SpanOperatorTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  auto cursor = std::unique_ptr<SpanOperatorTable::Cursor>(
      new SpanOperatorTable::Cursor(this, db_));
  int value = cursor->Initialize(qc, argv);
  return value != SQLITE_OK ? nullptr : std::move(cursor);
}

int SpanOperatorTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  // TODO(lalitm): figure out cost estimation.
  return SQLITE_OK;
}

std::vector<std::string> SpanOperatorTable::ComputeSqlConstraintVector(
    const QueryConstraints& qc,
    sqlite3_value** argv,
    ChildTable table) {
  std::vector<std::string> constraints;
  const auto& def = table == ChildTable::kFirst ? t1_defn_ : t2_defn_;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& constraint = qc.constraints()[i];

    std::string col_name;
    switch (constraint.iColumn) {
      case SpanOperatorTable::Column::kTimestamp:
        col_name = "ts";
        break;
      case SpanOperatorTable::Column::kDuration:
        col_name = "dur";
        break;
      default: {
        if (constraint.iColumn == SpanOperatorTable::Column::kJoinValue &&
            !join_col_.empty()) {
          col_name = join_col_;
          break;
        }
        auto index_pair = GetTableAndColumnIndex(constraint.iColumn);
        bool is_constraint_in_table = index_pair.first == table;
        if (is_constraint_in_table) {
          col_name = def.cols[index_pair.second].name();
        }
      }
    }

    if (!col_name.empty()) {
      auto value = sqlite_utils::SqliteValueAsString(argv[i]);
      constraints.emplace_back("`" + col_name + "`" +
                               OpToString(constraint.op) + value);
    }
  }
  return constraints;
}

std::pair<SpanOperatorTable::ChildTable, size_t>
SpanOperatorTable::GetTableAndColumnIndex(int joined_column_idx) {
  PERFETTO_CHECK(joined_column_idx >= kReservedColumns);

  size_t table_1_col =
      static_cast<size_t>(joined_column_idx - kReservedColumns);
  if (table_1_col < t1_defn_.cols.size()) {
    return std::make_pair(ChildTable::kFirst, table_1_col);
  }
  size_t table_2_col = table_1_col - t1_defn_.cols.size();
  PERFETTO_CHECK(table_2_col < t2_defn_.cols.size());
  return std::make_pair(ChildTable::kSecond, table_2_col);
}

SpanOperatorTable::Cursor::Cursor(SpanOperatorTable* table, sqlite3* db)
    : db_(db), table_(table) {}

int SpanOperatorTable::Cursor::Initialize(const QueryConstraints& qc,
                                          sqlite3_value** argv) {
  sqlite3_stmt* t1_raw = nullptr;
  int err =
      PrepareRawStmt(qc, argv, table_->t1_defn_, ChildTable::kFirst, &t1_raw);
  t1_.stmt.reset(t1_raw);
  if (err != SQLITE_OK)
    return err;

  sqlite3_stmt* t2_raw = nullptr;
  err =
      PrepareRawStmt(qc, argv, table_->t2_defn_, ChildTable::kSecond, &t2_raw);
  t2_.stmt.reset(t2_raw);
  if (err != SQLITE_OK)
    return err;

  // We step table 2 and allow Next() to step from table 1.
  next_stepped_table_ = ChildTable::kFirst;
  err = StepForTable(ChildTable::kSecond);

  // If there's no data in this table, then we are done without even looking
  // at the other table.
  if (err != SQLITE_ROW)
    return err == SQLITE_DONE ? SQLITE_OK : err;

  // Otherwise, find an overlapping span.
  return Next();
}

SpanOperatorTable::Cursor::~Cursor() {}

int SpanOperatorTable::Cursor::Next() {
  int err = StepForTable(next_stepped_table_);
  for (; err == SQLITE_ROW; err = StepForTable(next_stepped_table_)) {
    // Get both tables on the same join value.
    if (t1_.join_val < t2_.join_val) {
      next_stepped_table_ = ChildTable::kFirst;
      continue;
    } else if (t2_.join_val < t1_.join_val) {
      next_stepped_table_ = ChildTable::kSecond;
      continue;
    }

    // Get both tables to have an overlapping slice.
    if (t1_.ts_end <= t2_.ts_start || t1_.ts_start == t1_.ts_end) {
      next_stepped_table_ = ChildTable::kFirst;
      continue;
    } else if (t2_.ts_end <= t1_.ts_start || t2_.ts_start == t2_.ts_end) {
      next_stepped_table_ = ChildTable::kSecond;
      continue;
    }

    // Both slices now have an overlapping slice and the same join value.
    // Update the next stepped table to be the one which finishes earliest.
    next_stepped_table_ =
        t1_.ts_end <= t2_.ts_end ? ChildTable::kFirst : ChildTable::kSecond;
    return SQLITE_OK;
  }
  return err == SQLITE_DONE ? SQLITE_OK : err;
}

PERFETTO_ALWAYS_INLINE
int SpanOperatorTable::Cursor::StepForTable(ChildTable table) {
  TableState* pull_state = table == ChildTable::kFirst ? &t1_ : &t2_;
  auto* stmt = pull_state->stmt.get();

  int res = sqlite3_step(stmt);
  if (res == SQLITE_ROW) {
    int64_t ts = sqlite3_column_int64(stmt, Column::kTimestamp);
    int64_t dur = sqlite3_column_int64(stmt, Column::kDuration);
    int64_t join_val = sqlite3_column_int64(stmt, Column::kJoinValue);
    pull_state->ts_start = static_cast<uint64_t>(ts);
    pull_state->ts_end = pull_state->ts_start + static_cast<uint64_t>(dur);
    pull_state->join_val = join_val;
  } else if (res == SQLITE_DONE) {
    pull_state->ts_start = kU64Max;
    pull_state->ts_end = kU64Max;
    pull_state->join_val = kI64Max;
  }
  return res;
}

int SpanOperatorTable::Cursor::PrepareRawStmt(const QueryConstraints& qc,
                                              sqlite3_value** argv,
                                              const TableDefinition& def,
                                              ChildTable table,
                                              sqlite3_stmt** stmt) {
  // TODO(lalitm): pass through constraints on other tables to those tables.
  std::string sql;
  sql += "SELECT ts, dur, `" + table_->join_col_ + "`";
  for (const auto& col : def.cols) {
    sql += ", " + col.name();
  }
  sql += " FROM " + def.name;
  sql += " WHERE 1";
  auto cs = table_->ComputeSqlConstraintVector(qc, argv, table);
  for (const auto& c : cs) {
    sql += " AND " + c;
  }
  sql += " ORDER BY `" + table_->join_col_ + "`, ts;";

  PERFETTO_DLOG("%s", sql.c_str());
  int t1_size = static_cast<int>(sql.size());
  return sqlite3_prepare_v2(db_, sql.c_str(), t1_size, stmt, nullptr);
}

int SpanOperatorTable::Cursor::Eof() {
  return t1_.ts_start == kU64Max || t2_.ts_start == kU64Max;
}

int SpanOperatorTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kTimestamp: {
      auto max_ts = std::max(t1_.ts_start, t2_.ts_start);
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(max_ts));
      break;
    }
    case Column::kDuration: {
      auto max_start = std::max(t1_.ts_start, t2_.ts_start);
      auto min_end = std::min(t1_.ts_end, t2_.ts_end);
      PERFETTO_DCHECK(min_end > max_start);

      auto dur = min_end - max_start;
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(dur));
      break;
    }
    case Column::kJoinValue: {
      PERFETTO_DCHECK(t1_.join_val == t2_.join_val);
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(t1_.join_val));
      break;
    }
    default: {
      auto index_pair = table_->GetTableAndColumnIndex(N);
      const auto& stmt =
          index_pair.first == ChildTable::kFirst ? t1_.stmt : t2_.stmt;
      size_t index = index_pair.second + kReservedColumns;
      ReportSqliteResult(context, stmt.get(), index);
    }
  }
  return SQLITE_OK;
}

PERFETTO_ALWAYS_INLINE void SpanOperatorTable::Cursor::ReportSqliteResult(
    sqlite3_context* context,
    sqlite3_stmt* stmt,
    size_t index) {
  int idx = static_cast<int>(index);
  switch (sqlite3_column_type(stmt, idx)) {
    case SQLITE_INTEGER:
      sqlite3_result_int64(context, sqlite3_column_int64(stmt, idx));
      break;
    case SQLITE_FLOAT:
      sqlite3_result_double(context, sqlite3_column_double(stmt, idx));
      break;
    case SQLITE_TEXT: {
      // TODO(lalitm): note for future optimizations: if we knew the addresses
      // of the string intern pool, we could check if the string returned here
      // comes from the pool, and pass it as non-transient.
      const auto kSqliteTransient =
          reinterpret_cast<sqlite3_destructor_type>(-1);
      auto ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx));
      sqlite3_result_text(context, ptr, -1, kSqliteTransient);
      break;
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
