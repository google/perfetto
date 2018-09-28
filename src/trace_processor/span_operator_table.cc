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
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

using namespace sqlite_utils;

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

std::pair<bool, size_t> SpanOperatorTable::GetTableAndColumnIndex(
    int joined_column_idx) {
  PERFETTO_CHECK(joined_column_idx >= kReservedColumns);

  size_t table_1_col =
      static_cast<size_t>(joined_column_idx - kReservedColumns);
  if (table_1_col < t1_defn_.cols.size()) {
    return std::make_pair(true, table_1_col);
  }
  size_t table_2_col = table_1_col - t1_defn_.cols.size();
  PERFETTO_CHECK(table_2_col < t2_defn_.cols.size());
  return std::make_pair(false, table_2_col);
}

SpanOperatorTable::Cursor::Cursor(SpanOperatorTable* table, sqlite3* db)
    : db_(db), table_(table) {}

int SpanOperatorTable::Cursor::Initialize(const QueryConstraints& qc,
                                          sqlite3_value** argv) {
  sqlite3_stmt* t1_raw = nullptr;
  int err = PrepareRawStmt(qc, argv, table_->t1_defn_, true, &t1_raw);
  t1_.stmt.reset(t1_raw);
  if (err != SQLITE_OK)
    return err;

  sqlite3_stmt* t2_raw = nullptr;
  err = PrepareRawStmt(qc, argv, table_->t2_defn_, false, &t2_raw);
  t2_.stmt.reset(t2_raw);
  if (err != SQLITE_OK)
    return err;

  err = sqlite3_step(t1_.stmt.get());
  if (err != SQLITE_DONE) {
    if (err != SQLITE_ROW)
      return SQLITE_ERROR;
    int64_t ts = sqlite3_column_int64(t1_.stmt.get(), Column::kTimestamp);
    t1_.latest_ts = static_cast<uint64_t>(ts);
    t1_.col_count = static_cast<size_t>(sqlite3_column_count(t1_.stmt.get()));
  }

  err = sqlite3_step(t2_.stmt.get());
  if (err != SQLITE_DONE) {
    if (err != SQLITE_ROW)
      return SQLITE_ERROR;
    int64_t ts = sqlite3_column_int64(t2_.stmt.get(), Column::kTimestamp);
    t2_.latest_ts = static_cast<uint64_t>(ts);
    t2_.col_count = static_cast<size_t>(sqlite3_column_count(t2_.stmt.get()));
  }
  return Next();
}

SpanOperatorTable::Cursor::~Cursor() {}

int SpanOperatorTable::Cursor::PrepareRawStmt(const QueryConstraints& qc,
                                              sqlite3_value** argv,
                                              const TableDefinition& def,
                                              bool is_t1,
                                              sqlite3_stmt** stmt) {
  // TODO(lalitm): pass through constraints on other tables to those tables.
  std::string sql;
  sql += "SELECT ts, dur, " + table_->join_col_;
  for (const auto& col : def.cols) {
    sql += ", " + col.name();
  }
  sql += " FROM " + def.name;
  sql += " WHERE 1";

  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& constraint = qc.constraints()[i];
    int c = constraint.iColumn;
    std::string col_name;
    if (c == Column::kTimestamp) {
      col_name = "ts";
    } else if (c == Column::kDuration) {
      col_name = "dur";
    } else if (c == Column::kJoinValue) {
      col_name = table_->join_col_;
    } else {
      auto index_pair = table_->GetTableAndColumnIndex(c);
      bool is_constraint_in_current_table = index_pair.first == is_t1;
      if (is_constraint_in_current_table) {
        col_name = def.cols[index_pair.second].name();
      }
    }

    if (!col_name.empty()) {
      sql += " AND " + col_name + OpToString(constraint.op) +
             reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
    }
  }
  sql += " ORDER BY ts;";

  PERFETTO_DLOG("%s", sql.c_str());
  int t1_size = static_cast<int>(sql.size());
  return sqlite3_prepare_v2(db_, sql.c_str(), t1_size, stmt, nullptr);
}

int SpanOperatorTable::Cursor::Next() {
  PERFETTO_DCHECK(!intersecting_spans_.empty() || children_have_more_);

  // If there are no more rows to be added from the child tables, simply pop the
  // the front of the queue and return.
  if (!children_have_more_) {
    intersecting_spans_.pop_front();
    return SQLITE_OK;
  }

  // Remove the previously returned span but also try and find more
  // intersections.
  if (!intersecting_spans_.empty())
    intersecting_spans_.pop_front();

  // Pull from whichever cursor has the earlier timestamp and return if there
  // is a valid span.
  while (t1_.latest_ts < kU64Max || t2_.latest_ts < kU64Max) {
    int err = ExtractNext(t1_.latest_ts <= t2_.latest_ts);
    if (err == SQLITE_ROW) {
      return SQLITE_OK;
    } else if (err != SQLITE_DONE) {
      return err;
    }
  }

  // Once both cursors are completely exhausted, do one last pass through the
  // tables and return any final intersecting spans.
  for (auto it = t1_.spans.begin(); it != t1_.spans.end(); it++) {
    auto join_val = it->first;
    auto t2_it = t2_.spans.find(join_val);
    if (t2_it == t2_.spans.end())
      continue;
    MaybeAddIntersectingSpan(join_val, std::move(it->second),
                             std::move(t2_it->second));
  }

  // We don't have any more items to yield.
  children_have_more_ = false;
  return SQLITE_OK;
}

PERFETTO_ALWAYS_INLINE int SpanOperatorTable::Cursor::ExtractNext(
    bool pull_t1) {
  // Decide which table we will be retrieving a row from.
  TableState* pull_table = pull_t1 ? &t1_ : &t2_;

  // Extract the timestamp, duration and join value from that table.
  sqlite3_stmt* stmt = pull_table->stmt.get();
  int64_t ts = sqlite3_column_int64(stmt, Column::kTimestamp);
  int64_t dur = sqlite3_column_int64(stmt, Column::kDuration);
  int64_t join_val = sqlite3_column_int64(stmt, Column::kJoinValue);

  // Extract the actual row from the state.
  auto* pull_span = &pull_table->spans[join_val];

  // Save the old span (to allow us to return it) and then update the data in
  // the span.
  Span saved_span = std::move(*pull_span);
  pull_span->ts = static_cast<uint64_t>(ts);
  pull_span->dur = static_cast<uint64_t>(dur);
  pull_span->values.resize(pull_table->col_count - kReservedColumns);

  // Update all other columns.
  const auto& table_desc = pull_t1 ? table_->t1_defn_ : table_->t2_defn_;
  int col_count = static_cast<int>(pull_table->col_count);
  for (int i = kReservedColumns; i < col_count; i++) {
    size_t off = static_cast<size_t>(i - kReservedColumns);

    Value* value = &pull_span->values[off];
    value->type = table_desc.cols[off].type();
    switch (value->type) {
      case Table::ColumnType::kUlong:
        value->ulong_value =
            static_cast<uint64_t>(sqlite3_column_int64(stmt, i));
        break;
      case Table::ColumnType::kUint:
        value->uint_value = static_cast<uint32_t>(sqlite3_column_int(stmt, i));
        break;
      case Table::ColumnType::kString:
        value->text_value =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        break;
      case Table::ColumnType::kInt:
        PERFETTO_CHECK(false);
    }
  }

  // Get the next value from whichever table we just updated.
  int err = sqlite3_step(stmt);
  switch (err) {
    case SQLITE_DONE:
      pull_table->latest_ts = kU64Max;
      break;
    case SQLITE_ROW:
      pull_table->latest_ts =
          static_cast<uint64_t>(sqlite3_column_int64(stmt, Column::kTimestamp));
      break;
    default:
      return err;
  }

  // Create copies of the spans we want to intersect then perform the intersect.
  auto t1_span = pull_t1 ? std::move(saved_span) : t1_.spans[join_val];
  auto t2_span = pull_t1 ? t2_.spans[join_val] : std::move(saved_span);
  bool span_added = MaybeAddIntersectingSpan(join_val, t1_span, t2_span);
  return span_added ? SQLITE_ROW : SQLITE_DONE;
}

bool SpanOperatorTable::Cursor::MaybeAddIntersectingSpan(int64_t join_value,
                                                         Span t1_span,
                                                         Span t2_span) {
  uint64_t t1_end = t1_span.ts + t1_span.dur;
  uint64_t t2_end = t2_span.ts + t2_span.dur;

  // If there is no overlap between the two spans, don't return anything.
  if (t1_end == 0 || t2_end == 0 || t2_end < t1_span.ts || t1_end < t2_span.ts)
    return false;

  IntersectingSpan value;
  value.ts = std::max(t1_span.ts, t2_span.ts);
  value.dur = std::min(t1_end, t2_end) - value.ts;
  value.join_val = join_value;
  value.t1_span = std::move(t1_span);
  value.t2_span = std::move(t2_span);
  intersecting_spans_.emplace_back(std::move(value));

  return true;
}

int SpanOperatorTable::Cursor::Eof() {
  return intersecting_spans_.empty() && !children_have_more_;
}

int SpanOperatorTable::Cursor::Column(sqlite3_context* context, int N) {
  const auto& ret = intersecting_spans_.front();
  switch (N) {
    case Column::kTimestamp:
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(ret.ts));
      break;
    case Column::kDuration:
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(ret.dur));
      break;
    case Column::kJoinValue:
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(ret.join_val));
      break;
    default: {
      auto index_pair = table_->GetTableAndColumnIndex(N);
      const auto& row = index_pair.first ? ret.t1_span : ret.t2_span;
      ReportSqliteResult(context, row.values[index_pair.second]);
    }
  }
  return SQLITE_OK;
}

PERFETTO_ALWAYS_INLINE void SpanOperatorTable::Cursor::ReportSqliteResult(
    sqlite3_context* context,
    SpanOperatorTable::Value value) {
  switch (value.type) {
    case Table::ColumnType::kUint:
      sqlite3_result_int(context, static_cast<int>(value.uint_value));
      break;
    case Table::ColumnType::kUlong:
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(value.ulong_value));
      break;
    case Table::ColumnType::kString: {
      // Note: If you could guarantee that you never sqlite3_step() the cursor
      // before accessing the values here, you could avoid string copies and
      // pass through the const char* obtained in ExtractNext
      const auto kSqliteTransient =
          reinterpret_cast<sqlite3_destructor_type>(-1);
      sqlite3_result_text(context, value.text_value.c_str(), -1,
                          kSqliteTransient);
      break;
    }
    case Table::ColumnType::kInt:
      PERFETTO_CHECK(false);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
