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

#include "src/trace_processor/span_join_operator_table.h"

#include <sqlite3.h>
#include <string.h>
#include <algorithm>
#include <set>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/base/string_view.h"
#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();

constexpr char kTsColumnName[] = "ts";
constexpr char kDurColumnName[] = "dur";

bool IsRequiredColumn(const std::string& name) {
  return name == kTsColumnName || name == kDurColumnName;
}

void CheckRequiredColumns(const std::vector<Table::Column>& cols) {
  int required_columns_found = 0;
  for (const auto& col : cols) {
    if (IsRequiredColumn(col.name())) {
      ++required_columns_found;
      if (col.type() != Table::ColumnType::kUlong &&
          col.type() != Table::ColumnType::kUnknown) {
        PERFETTO_ELOG("Invalid column type for %s", col.name().c_str());
      }
    }
  }
  if (required_columns_found != 2) {
    PERFETTO_ELOG("Required columns not found (found %d)",
                  required_columns_found);
  }
}
}  // namespace

SpanJoinOperatorTable::SpanJoinOperatorTable(sqlite3* db, const TraceStorage*)
    : db_(db) {}

void SpanJoinOperatorTable::RegisterTable(sqlite3* db,
                                          const TraceStorage* storage) {
  Table::Register<SpanJoinOperatorTable>(db, storage, "span_join",
                                         /* read_write */ false,
                                         /* requires_args */ true);
}

Table::Schema SpanJoinOperatorTable::CreateSchema(int argc,
                                                  const char* const* argv) {
  // argv[0] - argv[2] are SQLite populated fields which are always present.
  if (argc < 5) {
    PERFETTO_ELOG("SPAN JOIN expected at least 2 args, received %d", argc - 3);
    return Table::Schema({}, {});
  }

  std::string t1_raw_desc = reinterpret_cast<const char*>(argv[3]);
  auto t1_desc = TableDescriptor::Parse(t1_raw_desc);

  std::string t2_raw_desc = reinterpret_cast<const char*>(argv[4]);
  auto t2_desc = TableDescriptor::Parse(t2_raw_desc);

  // For now, ensure that both tables are partitioned by the same column.
  // TODO(lalitm): relax this constraint.
  PERFETTO_CHECK(t1_desc.partition_col == t2_desc.partition_col);

  // TODO(lalitm): add logic to ensure that the tables that are being joined
  // are actually valid to be joined i.e. they have the same partition.
  auto t1_cols = sqlite_utils::GetColumnsForTable(db_, t1_desc.name);
  CheckRequiredColumns(t1_cols);
  auto t2_cols = sqlite_utils::GetColumnsForTable(db_, t2_desc.name);
  CheckRequiredColumns(t2_cols);

  t1_defn_ = TableDefinition(t1_desc.name, t1_desc.partition_col, t1_cols);
  t2_defn_ = TableDefinition(t2_desc.name, t2_desc.partition_col, t2_cols);

  std::vector<Table::Column> cols;
  cols.emplace_back(Column::kTimestamp, kTsColumnName, ColumnType::kUlong);
  cols.emplace_back(Column::kDuration, kDurColumnName, ColumnType::kUlong);

  is_same_partition_ = t1_desc.partition_col == t2_desc.partition_col;
  const auto& partition_col = t1_desc.partition_col;
  if (is_same_partition_)
    cols.emplace_back(Column::kPartition, partition_col, ColumnType::kLong);

  CreateSchemaColsForDefn(t1_defn_, &cols);
  CreateSchemaColsForDefn(t2_defn_, &cols);

  return Schema(cols, {Column::kTimestamp, Column::kPartition});
}

void SpanJoinOperatorTable::CreateSchemaColsForDefn(
    const TableDefinition& defn,
    std::vector<Table::Column>* cols) {
  for (size_t i = 0; i < defn.columns().size(); i++) {
    const auto& n = defn.columns()[i].name();
    if (IsRequiredColumn(n))
      continue;
    if (n == defn.partition_col() && is_same_partition_)
      continue;

    ColumnLocator* locator = &global_index_to_column_locator_[cols->size()];
    locator->defn = &defn;
    locator->col_index = i;

    cols->emplace_back(cols->size(), n, defn.columns()[i].type());
  }
}

std::unique_ptr<Table::Cursor> SpanJoinOperatorTable::CreateCursor(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  auto cursor = std::unique_ptr<SpanJoinOperatorTable::Cursor>(
      new SpanJoinOperatorTable::Cursor(this, db_));
  int value = cursor->Initialize(qc, argv);
  return value != SQLITE_OK ? nullptr : std::move(cursor);
}

int SpanJoinOperatorTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  // TODO(lalitm): figure out cost estimation.
  return SQLITE_OK;
}

std::vector<std::string>
SpanJoinOperatorTable::ComputeSqlConstraintsForDefinition(
    const TableDefinition& defn,
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  std::vector<std::string> constraints;
  for (size_t i = 0; i < qc.constraints().size(); i++) {
    const auto& cs = qc.constraints()[i];
    auto col_name = GetNameForGlobalColumnIndex(defn, cs.iColumn);
    if (col_name == "")
      continue;

    if (col_name == kTsColumnName || col_name == kDurColumnName) {
      // We don't support constraints on ts or duration in the child tables.
      PERFETTO_DCHECK(false);
      continue;
    }
    auto op = sqlite_utils::OpToString(cs.op);
    auto value = sqlite_utils::SqliteValueAsString(argv[i]);

    constraints.emplace_back("`" + col_name + "`" + op + value);
  }
  return constraints;
}

std::string SpanJoinOperatorTable::GetNameForGlobalColumnIndex(
    const TableDefinition& defn,
    int global_column) {
  size_t col_idx = static_cast<size_t>(global_column);
  if (col_idx == Column::kTimestamp)
    return kTsColumnName;
  else if (col_idx == Column::kDuration)
    return kDurColumnName;
  else if (is_same_partition_ && col_idx == Column::kPartition)
    return defn.partition_col().c_str();

  const auto& locator = global_index_to_column_locator_[col_idx];
  if (locator.defn != &defn)
    return "";
  return defn.columns()[locator.col_index].name().c_str();
}

SpanJoinOperatorTable::Cursor::Cursor(SpanJoinOperatorTable* table, sqlite3* db)
    : t1_(table, &table->t1_defn_, db),
      t2_(table, &table->t2_defn_, db),
      table_(table) {}

int SpanJoinOperatorTable::Cursor::Initialize(const QueryConstraints& qc,
                                              sqlite3_value** argv) {
  int err = t1_.Initialize(qc, argv);
  if (err != SQLITE_OK)
    return err;

  err = t2_.Initialize(qc, argv);
  if (err != SQLITE_OK)
    return err;

  // We step table 2 and allow Next() to step from table 1.
  next_stepped_table_ = &t1_;
  err = t2_.StepAndCacheValues();

  // If there's no data in this table, then we are done without even looking
  // at the other table.
  if (err != SQLITE_ROW)
    return err == SQLITE_DONE ? SQLITE_OK : err;

  // Otherwise, find an overlapping span.
  return Next();
}

SpanJoinOperatorTable::Cursor::~Cursor() {}

int SpanJoinOperatorTable::Cursor::Next() {
  int err = next_stepped_table_->StepAndCacheValues();
  for (; err == SQLITE_ROW; err = next_stepped_table_->StepAndCacheValues()) {
    // Get both tables on the same parition.
    if (t1_.partition() < t2_.partition()) {
      next_stepped_table_ = &t1_;
      continue;
    } else if (t2_.partition() < t1_.partition()) {
      next_stepped_table_ = &t2_;
      continue;
    }

    // Get both tables to have an overlapping slice.
    if (t1_.ts_end() <= t2_.ts_start() || t1_.ts_start() == t1_.ts_end()) {
      next_stepped_table_ = &t1_;
      continue;
    } else if (t2_.ts_end() <= t1_.ts_start() ||
               t2_.ts_start() == t2_.ts_end()) {
      next_stepped_table_ = &t2_;
      continue;
    }

    // Both slices now have an overlapping slice and the same partition.
    // Update the next stepped table to be the one which finishes earliest.
    next_stepped_table_ = t1_.ts_end() <= t2_.ts_end() ? &t1_ : &t2_;
    return SQLITE_OK;
  }
  return err == SQLITE_DONE ? SQLITE_OK : err;
}

int SpanJoinOperatorTable::Cursor::Eof() {
  return t1_.ts_start() == kI64Max || t2_.ts_start() == kI64Max;
}

int SpanJoinOperatorTable::Cursor::Column(sqlite3_context* context, int N) {
  switch (N) {
    case Column::kTimestamp: {
      auto max_ts = std::max(t1_.ts_start(), t2_.ts_start());
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(max_ts));
      break;
    }
    case Column::kDuration: {
      auto max_start = std::max(t1_.ts_start(), t2_.ts_start());
      auto min_end = std::min(t1_.ts_end(), t2_.ts_end());
      PERFETTO_DCHECK(min_end > max_start);

      auto dur = min_end - max_start;
      sqlite3_result_int64(context, static_cast<sqlite3_int64>(dur));
      break;
    }
    case Column::kPartition: {
      PERFETTO_DCHECK(t1_.partition() == t2_.partition());
      sqlite3_result_int64(context,
                           static_cast<sqlite3_int64>(t1_.partition()));
      break;
    }
    default: {
      size_t index = static_cast<size_t>(N);
      const auto& locator = table_->global_index_to_column_locator_[index];
      if (locator.defn == t1_.definition())
        t1_.ReportSqliteResult(context, locator.col_index);
      else
        t2_.ReportSqliteResult(context, locator.col_index);
      break;
    }
  }
  return SQLITE_OK;
}

SpanJoinOperatorTable::Cursor::TableQueryState::TableQueryState(
    SpanJoinOperatorTable* table,
    const TableDefinition* definition,
    sqlite3* db)
    : defn_(definition), db_(db), table_(table) {}

int SpanJoinOperatorTable::Cursor::TableQueryState::Initialize(
    const QueryConstraints& qc,
    sqlite3_value** argv) {
  auto cs = table_->ComputeSqlConstraintsForDefinition(*defn_, qc, argv);
  return PrepareRawStmt(CreateSqlQuery(cs));
}

int SpanJoinOperatorTable::Cursor::TableQueryState::StepAndCacheValues() {
  sqlite3_stmt* stmt = stmt_.get();

  // Fastforward through any rows with null partition keys.
  int res, row_type;
  do {
    res = sqlite3_step(stmt);
    row_type = sqlite3_column_type(stmt, Column::kPartition);
  } while (res == SQLITE_ROW && row_type == SQLITE_NULL);

  if (res == SQLITE_ROW) {
    int64_t ts = sqlite3_column_int64(stmt, Column::kTimestamp);
    int64_t dur = sqlite3_column_int64(stmt, Column::kDuration);
    int64_t partition = sqlite3_column_int64(stmt, Column::kPartition);
    ts_start_ = ts;
    ts_end_ = ts_start_ + dur;
    partition_ = partition;
  } else if (res == SQLITE_DONE) {
    ts_start_ = kI64Max;
    ts_end_ = kI64Max;
    partition_ = kI64Max;
  }
  return res;
}

std::string SpanJoinOperatorTable::Cursor::TableQueryState::CreateSqlQuery(
    const std::vector<std::string>& cs) {
  // TODO(lalitm): pass through constraints on other tables to those tables.
  std::string sql;
  sql += "SELECT ts, dur, `" + defn_->partition_col() + "`";
  for (const auto& col : defn_->columns()) {
    if (IsRequiredColumn(col.name()) || col.name() == defn_->partition_col())
      continue;
    sql += ", " + col.name();
  }
  sql += " FROM " + defn_->name();
  sql += " WHERE 1";
  for (const auto& c : cs) {
    sql += " AND " + c;
  }
  sql += " ORDER BY `" + defn_->partition_col() + "`, ts;";
  return sql;
}

int SpanJoinOperatorTable::Cursor::TableQueryState::PrepareRawStmt(
    const std::string& sql) {
  PERFETTO_DLOG("%s", sql.c_str());
  int size = static_cast<int>(sql.size());

  sqlite3_stmt* stmt = nullptr;
  int err = sqlite3_prepare_v2(db_, sql.c_str(), size, &stmt, nullptr);
  stmt_.reset(stmt);
  return err;
}

void SpanJoinOperatorTable::Cursor::TableQueryState::ReportSqliteResult(
    sqlite3_context* context,
    size_t index) {
  sqlite3_stmt* stmt = stmt_.get();
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

SpanJoinOperatorTable::TableDefinition::TableDefinition(
    std::string name,
    std::string partition_col,
    std::vector<Table::Column> cols)
    : name_(std::move(name)),
      partition_col_(std::move(partition_col)),
      cols_(std::move(cols)) {}

SpanJoinOperatorTable::TableDescriptor
SpanJoinOperatorTable::TableDescriptor::Parse(
    const std::string& raw_descriptor) {
  // Descriptors have one of the following forms:
  // table_name PARTITIONED column_name

  // Find the table name. Note we don't support not specifying a partition
  // column at the moment.
  base::StringSplitter splitter(raw_descriptor, ' ');
  if (!splitter.Next())
    return {};

  std::string name = splitter.cur_token();
  if (!splitter.Next())
    return {};
  if (strcmp(splitter.cur_token(), "PARTITIONED") != 0)
    return {};
  if (!splitter.Next())
    return {};

  std::string partition_col = splitter.cur_token();

  TableDescriptor descriptor;
  descriptor.name = std::move(name);
  descriptor.partition_col = std::move(partition_col);
  return descriptor;
}

}  // namespace trace_processor
}  // namespace perfetto
