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
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/base/string_utils.h"
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

}  // namespace

SpanJoinOperatorTable::SpanJoinOperatorTable(sqlite3* db, const TraceStorage*)
    : db_(db) {}

void SpanJoinOperatorTable::RegisterTable(sqlite3* db,
                                          const TraceStorage* storage) {
  Table::Register<SpanJoinOperatorTable>(db, storage, "span_join",
                                         /* read_write */ false,
                                         /* requires_args */ true);
}

base::Optional<Table::Schema> SpanJoinOperatorTable::Init(
    int argc,
    const char* const* argv) {
  // argv[0] - argv[2] are SQLite populated fields which are always present.
  if (argc < 5) {
    PERFETTO_ELOG("SPAN JOIN expected at least 2 args, received %d", argc - 3);
    return base::nullopt;
  }

  auto maybe_t1_desc = TableDescriptor::Parse(
      std::string(reinterpret_cast<const char*>(argv[3])));
  if (!maybe_t1_desc.has_value())
    return base::nullopt;
  auto t1_desc = *maybe_t1_desc;

  auto maybe_t2_desc = TableDescriptor::Parse(
      std::string(reinterpret_cast<const char*>(argv[4])));
  if (!maybe_t2_desc.has_value())
    return base::nullopt;
  auto t2_desc = *maybe_t2_desc;

  // If we're in the mixed case, ensure t1 is the partitioned table.
  if (t1_desc.partition_col.empty() && !t2_desc.partition_col.empty()) {
    std::swap(t1_desc, t2_desc);
  }

  auto maybe_t1_defn = CreateTableDefinition(t1_desc);
  if (!maybe_t1_defn.has_value())
    return base::nullopt;
  t1_defn_ = maybe_t1_defn.value();

  auto maybe_t2_defn = CreateTableDefinition(t2_desc);
  if (!maybe_t2_defn.has_value())
    return base::nullopt;
  t2_defn_ = maybe_t2_defn.value();

  if (t1_desc.partition_col == t2_desc.partition_col) {
    partitioning_ = t1_desc.partition_col.empty()
                        ? PartitioningType::kNoPartitioning
                        : PartitioningType::kSamePartitioning;
  } else if (t1_defn_.IsPartitioned() && t2_defn_.IsPartitioned()) {
    PERFETTO_ELOG("Mismatching partitions (%s, %s)",
                  t1_defn_.partition_col().c_str(),
                  t2_defn_.partition_col().c_str());
    return base::nullopt;
  } else {
    partitioning_ = PartitioningType::kMixedPartitioning;
  }

  std::vector<Table::Column> cols;
  // Ensure the shared columns are consistently ordered and are not
  // present twice in the final schema
  cols.emplace_back(Column::kTimestamp, kTsColumnName, ColumnType::kLong);
  cols.emplace_back(Column::kDuration, kDurColumnName, ColumnType::kLong);
  if (partitioning_ != PartitioningType::kNoPartitioning)
    cols.emplace_back(Column::kPartition, t1_desc.partition_col,
                      ColumnType::kLong);

  CreateSchemaColsForDefn(t1_defn_, &cols);
  CreateSchemaColsForDefn(t2_defn_, &cols);

  std::vector<size_t> primary_keys = {Column::kTimestamp};
  if (partitioning_ != PartitioningType::kNoPartitioning) {
    primary_keys.push_back(Column::kPartition);
  }
  return Schema(cols, primary_keys);
}

void SpanJoinOperatorTable::CreateSchemaColsForDefn(
    const TableDefinition& defn,
    std::vector<Table::Column>* cols) {
  for (size_t i = 0; i < defn.columns().size(); i++) {
    const auto& n = defn.columns()[i].name();
    if (IsRequiredColumn(n) || n == defn.partition_col())
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
  auto cursor = partitioning_ == PartitioningType::kMixedPartitioning
                    ? std::unique_ptr<SpanJoinOperatorTable::Cursor>(
                          new MixedPartitioningCursor(this, db_))
                    : std::unique_ptr<SpanJoinOperatorTable::Cursor>(
                          new SinglePartitioningCursor(this, db_));
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
      PERFETTO_DFATAL("ts or duration constraints on child tables");
      continue;
    }
    auto op = sqlite_utils::OpToString(cs.op);
    auto value = sqlite_utils::SqliteValueAsString(argv[i]);

    constraints.emplace_back("`" + col_name + "`" + op + value);
  }
  return constraints;
}

base::Optional<SpanJoinOperatorTable::TableDefinition>
SpanJoinOperatorTable::CreateTableDefinition(const TableDescriptor& desc) {
  auto cols = sqlite_utils::GetColumnsForTable(db_, desc.name);

  uint32_t required_columns_found = 0;
  uint32_t ts_idx = std::numeric_limits<uint32_t>::max();
  uint32_t dur_idx = std::numeric_limits<uint32_t>::max();
  uint32_t partition_idx = std::numeric_limits<uint32_t>::max();
  for (uint32_t i = 0; i < cols.size(); i++) {
    auto col = cols[i];
    if (IsRequiredColumn(col.name())) {
      ++required_columns_found;
      if (col.type() != Table::ColumnType::kLong &&
          col.type() != Table::ColumnType::kUnknown) {
        PERFETTO_ELOG("Invalid column type for %s", col.name().c_str());
        return base::nullopt;
      }
    }

    if (col.name() == kTsColumnName) {
      ts_idx = i;
    } else if (col.name() == kDurColumnName) {
      dur_idx = i;
    } else if (col.name() == desc.partition_col) {
      partition_idx = i;
    }
  }
  if (required_columns_found != 2) {
    PERFETTO_ELOG("Required columns not found (found %d)",
                  required_columns_found);
    return base::nullopt;
  }

  PERFETTO_DCHECK(ts_idx < cols.size());
  PERFETTO_DCHECK(dur_idx < cols.size());
  PERFETTO_DCHECK(desc.partition_col.empty() || partition_idx < cols.size());

  return TableDefinition(desc.name, desc.partition_col, std::move(cols), ts_idx,
                         dur_idx, partition_idx);
}

std::string SpanJoinOperatorTable::GetNameForGlobalColumnIndex(
    const TableDefinition& defn,
    int global_column) {
  size_t col_idx = static_cast<size_t>(global_column);
  if (col_idx == Column::kTimestamp)
    return kTsColumnName;
  else if (col_idx == Column::kDuration)
    return kDurColumnName;
  else if (col_idx == Column::kPartition &&
           partitioning_ != PartitioningType::kNoPartitioning)
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

bool SpanJoinOperatorTable::Cursor::IsOverlappingSpan(
    TableQueryState* t1,
    TableQueryState* t2,
    TableQueryState** next_stepped_table) {
  // Get both tables to have an overlapping slice.
  if (t1->ts_end() <= t2->ts_start() || t1->ts_start() == t1->ts_end()) {
    *next_stepped_table = t1;
    return false;
  }

  if (t2->ts_end() <= t1->ts_start() || t2->ts_start() == t2->ts_end()) {
    *next_stepped_table = t2;
    return false;
  }

  // Both slices now have an overlapping slice and the same partition.
  // Update the next stepped table to be the one which finishes earliest.
  *next_stepped_table = t1->ts_end() <= t2->ts_end() ? t1 : t2;
  return true;
}

SpanJoinOperatorTable::SinglePartitioningCursor::SinglePartitioningCursor(
    SpanJoinOperatorTable* table,
    sqlite3* db)
    : Cursor(table, db) {
  PERFETTO_DCHECK(t1_.definition()->partition_col() ==
                  t2_.definition()->partition_col());
}

int SpanJoinOperatorTable::SinglePartitioningCursor::Next() {
  while (true) {
    int err = next_stepped_table_->StepAndCacheValues();
    // TODO: Propagate error msg to the table.
    if (err != SQLITE_ROW && err != SQLITE_DONE)
      return err;
    if (Eof())
      break;

    if (t1_.partition() < t2_.partition()) {
      next_stepped_table_ = &t1_;
      continue;
    } else if (t2_.partition() < t1_.partition()) {
      next_stepped_table_ = &t2_;
      continue;
    }

    if (IsOverlappingSpan(&t1_, &t2_, &next_stepped_table_))
      return SQLITE_OK;
  }
  // EOF
  return SQLITE_OK;
}

int SpanJoinOperatorTable::SinglePartitioningCursor::Eof() {
  return t1_.Eof() || t2_.Eof();
}

SpanJoinOperatorTable::MixedPartitioningCursor::MixedPartitioningCursor(
    SpanJoinOperatorTable* table,
    sqlite3* db)
    : Cursor(table, db) {
  PERFETTO_DCHECK(t1_.definition()->IsPartitioned() &&
                  !t2_.definition()->IsPartitioned());
}

int SpanJoinOperatorTable::MixedPartitioningCursor::Next() {
  while (true) {
    int64_t prev_partition = t1_.partition();
    int err = next_stepped_table_->StepAndCacheValues();
    // TODO: Propagate error msg to the table.
    if (err != SQLITE_ROW && err != SQLITE_DONE)
      return err;

    // For mixed partitioning, Eof == t1.Eof
    if (Eof())
      break;

    // t1 switched partitions, rewind the unpartitioned table.
    if (t1_.partition() != prev_partition) {
      int reset_err = t2_.PrepareRawStmt();
      if (reset_err != SQLITE_OK)
        return reset_err;
      next_stepped_table_ = &t2_;
      continue;
    }

    // t2 is out of data, fast forward t1
    if (t2_.Eof()) {
      next_stepped_table_ = &t1_;
      continue;
    }

    if (IsOverlappingSpan(&t1_, &t2_, &next_stepped_table_))
      return SQLITE_OK;
  }
  // EOF
  return SQLITE_OK;
}

int SpanJoinOperatorTable::MixedPartitioningCursor::Eof() {
  return t1_.Eof();
}

int SpanJoinOperatorTable::Cursor::Column(sqlite3_context* context, int N) {
  PERFETTO_DCHECK(!t1_.Eof());
  PERFETTO_DCHECK(!t2_.Eof());
  if (N == Column::kTimestamp) {
    auto max_ts = std::max(t1_.ts_start(), t2_.ts_start());
    sqlite3_result_int64(context, static_cast<sqlite3_int64>(max_ts));
  } else if (N == Column::kDuration) {
    auto max_start = std::max(t1_.ts_start(), t2_.ts_start());
    auto min_end = std::min(t1_.ts_end(), t2_.ts_end());
    PERFETTO_DCHECK(min_end > max_start);
    auto dur = min_end - max_start;
    sqlite3_result_int64(context, static_cast<sqlite3_int64>(dur));
  } else if (N == Column::kPartition &&
             table_->partitioning_ != PartitioningType::kNoPartitioning) {
    PERFETTO_DCHECK(table_->partitioning_ ==
                        PartitioningType::kMixedPartitioning ||
                    t1_.partition() == t2_.partition());
    sqlite3_result_int64(context, static_cast<sqlite3_int64>(t1_.partition()));
  } else {
    size_t index = static_cast<size_t>(N);
    const auto& locator = table_->global_index_to_column_locator_[index];
    if (locator.defn == t1_.definition())
      t1_.ReportSqliteResult(context, locator.col_index);
    else
      t2_.ReportSqliteResult(context, locator.col_index);
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
  sql_query_ = CreateSqlQuery(
      table_->ComputeSqlConstraintsForDefinition(*defn_, qc, argv));
  return PrepareRawStmt();
}

int SpanJoinOperatorTable::Cursor::TableQueryState::StepAndCacheValues() {
  sqlite3_stmt* stmt = stmt_.get();

  auto ts_idx = static_cast<int>(definition()->ts_idx());
  auto dur_idx = static_cast<int>(definition()->dur_idx());
  auto partition_idx = static_cast<int>(definition()->partition_idx());
  PERFETTO_DCHECK(!definition()->IsPartitioned() ||
                  static_cast<size_t>(partition_idx) <
                      definition()->columns().size());

  int res;
  if (definition()->IsPartitioned()) {
    // Fastforward through any rows with null partition keys.
    int row_type;
    do {
      res = sqlite3_step(stmt);
      row_type = sqlite3_column_type(stmt, partition_idx);
    } while (res == SQLITE_ROW && row_type == SQLITE_NULL);
  } else {
    res = sqlite3_step(stmt);
  }

  if (res == SQLITE_ROW) {
    int64_t ts = sqlite3_column_int64(stmt, ts_idx);
    int64_t dur = sqlite3_column_int64(stmt, dur_idx);
    ts_start_ = ts;
    ts_end_ = ts_start_ + dur;
    if (definition()->IsPartitioned()) {
      partition_ = sqlite3_column_int64(stmt, partition_idx);
    }
  } else if (res == SQLITE_DONE) {
    ts_start_ = kI64Max;
    ts_end_ = kI64Max;
    partition_ = kI64Max;
  }
  return res;
}

std::string SpanJoinOperatorTable::Cursor::TableQueryState::CreateSqlQuery(
    const std::vector<std::string>& cs) const {
  std::vector<std::string> col_names;
  for (const Table::Column& c : defn_->columns()) {
    col_names.push_back("`" + c.name() + "`");
  }

  std::string sql = "SELECT " + base::Join(col_names, ", ");
  sql += " FROM " + defn_->name();
  if (!cs.empty()) {
    sql += " WHERE " + base::Join(cs, " AND ");
  }
  sql += " ORDER BY ";
  sql += defn_->IsPartitioned()
             ? base::Join({"`" + defn_->partition_col() + "`", "ts"}, ", ")
             : "ts";
  sql += ";";
  PERFETTO_DLOG("%s", sql.c_str());
  return sql;
}

int SpanJoinOperatorTable::Cursor::TableQueryState::PrepareRawStmt() {
  sqlite3_stmt* stmt = nullptr;
  int err =
      sqlite3_prepare_v2(db_, sql_query_.c_str(),
                         static_cast<int>(sql_query_.size()), &stmt, nullptr);
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
    std::vector<Table::Column> cols,
    uint32_t ts_idx,
    uint32_t dur_idx,
    uint32_t partition_idx)
    : name_(std::move(name)),
      partition_col_(std::move(partition_col)),
      cols_(std::move(cols)),
      ts_idx_(ts_idx),
      dur_idx_(dur_idx),
      partition_idx_(partition_idx) {}

base::Optional<SpanJoinOperatorTable::TableDescriptor>
SpanJoinOperatorTable::TableDescriptor::Parse(
    const std::string& raw_descriptor) {
  // Descriptors have one of the following forms:
  // table_name [PARTITIONED column_name]

  // Find the table name.
  base::StringSplitter splitter(raw_descriptor, ' ');
  if (!splitter.Next())
    return base::nullopt;

  TableDescriptor descriptor;
  descriptor.name = splitter.cur_token();
  if (!splitter.Next())
    return std::move(descriptor);

  if (strcasecmp(splitter.cur_token(), "PARTITIONED") != 0) {
    PERFETTO_ELOG("Invalid SPAN_JOIN token %s", splitter.cur_token());
    return base::nullopt;
  }
  if (!splitter.Next()) {
    PERFETTO_ELOG("Missing partitioning column");
    return base::nullopt;
  }

  descriptor.partition_col = splitter.cur_token();
  return std::move(descriptor);
}

}  // namespace trace_processor
}  // namespace perfetto
