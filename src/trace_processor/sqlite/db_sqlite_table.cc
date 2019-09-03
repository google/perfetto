/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

FilterOp SqliteOpToFilterOp(int sqlite_op) {
  switch (sqlite_op) {
    case SQLITE_INDEX_CONSTRAINT_EQ:
    case SQLITE_INDEX_CONSTRAINT_IS:
      return FilterOp::kEq;
    case SQLITE_INDEX_CONSTRAINT_GT:
      return FilterOp::kGt;
    case SQLITE_INDEX_CONSTRAINT_LT:
      return FilterOp::kLt;
    default:
      PERFETTO_FATAL("Currently unsupported constraint");
  }
}

SqlValue SqliteValueToSqlValue(sqlite3_value* sqlite_val) {
  auto col_type = sqlite3_value_type(sqlite_val);
  SqlValue value;
  switch (col_type) {
    case SQLITE_INTEGER:
      value.type = SqlValue::kLong;
      value.long_value = sqlite3_value_int64(sqlite_val);
      break;
    case SQLITE_TEXT:
      value.type = SqlValue::kString;
      value.string_value =
          reinterpret_cast<const char*>(sqlite3_value_text(sqlite_val));
      break;
    case SQLITE_FLOAT:
      value.type = SqlValue::kDouble;
      value.double_value = sqlite3_value_double(sqlite_val);
      break;
    case SQLITE_BLOB:
      value.type = SqlValue::kBytes;
      value.bytes_value = sqlite3_value_blob(sqlite_val);
      value.bytes_count = static_cast<size_t>(sqlite3_value_bytes(sqlite_val));
      break;
    case SQLITE_NULL:
      value.type = SqlValue::kNull;
      break;
  }
  return value;
}

}  // namespace

DbSqliteTable::DbSqliteTable(sqlite3*, const Table* table) : table_(table) {}
DbSqliteTable::~DbSqliteTable() = default;

void DbSqliteTable::RegisterTable(sqlite3* db,
                                  const Table* table,
                                  const std::string& name) {
  SqliteTable::Register<DbSqliteTable, const Table*>(db, table, name);
}

util::Status DbSqliteTable::Init(int, const char* const*, Schema* schema) {
  std::vector<SqliteTable::Column> schema_cols;
  for (uint32_t i = 0; i < table_->GetColumnCount(); ++i) {
    const auto& col = table_->GetColumn(i);
    schema_cols.emplace_back(i, col.name(), col.type());
  }
  // TODO(lalitm): this is hardcoded to be the id column but change this to be
  // more generic in the future.
  auto opt_idx = table_->FindColumnIdxByName("id");
  if (!opt_idx) {
    PERFETTO_FATAL(
        "id column not found in %s. Currently all db Tables need to contain an "
        "id column; this constraint will be relaxed in the future.",
        name().c_str());
  }

  std::vector<size_t> primary_keys;
  primary_keys.emplace_back(*opt_idx);

  *schema = Schema(std::move(schema_cols), std::move(primary_keys));
  return util::OkStatus();
}

int DbSqliteTable::BestIndex(const QueryConstraints&, BestIndexInfo*) {
  // TODO(lalitm): add proper cost estimation by looking at the columns.
  return SQLITE_OK;
}

std::unique_ptr<SqliteTable::Cursor> DbSqliteTable::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this));
}

DbSqliteTable::Cursor::Cursor(DbSqliteTable* table)
    : SqliteTable::Cursor(table), initial_db_table_(table->table_) {}

int DbSqliteTable::Cursor::Filter(const QueryConstraints& qc,
                                  sqlite3_value** argv) {
  // We reuse this vector to reduce memory allocations on nested subqueries.
  constraints_.resize(qc.constraints().size());
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];
    uint32_t col = static_cast<uint32_t>(cs.iColumn);

    FilterOp op = SqliteOpToFilterOp(cs.op);
    SqlValue value = SqliteValueToSqlValue(argv[i]);

    constraints_[i] = Constraint{col, op, value};
  }

  // We reuse this vector to reduce memory allocations on nested subqueries.
  orders_.resize(qc.order_by().size());
  for (size_t i = 0; i < qc.order_by().size(); ++i) {
    const auto& ob = qc.order_by()[i];
    uint32_t col = static_cast<uint32_t>(ob.iColumn);
    orders_[i] = Order{col, static_cast<bool>(ob.desc)};
  }

  db_table_ = initial_db_table_->Filter(constraints_).Sort(orders_);
  iterator_ = db_table_->IterateRows();

  return Next();
}

int DbSqliteTable::Cursor::Next() {
  eof_ = !iterator_->Next();
  return SQLITE_OK;
}

int DbSqliteTable::Cursor::Eof() {
  return eof_;
}

int DbSqliteTable::Cursor::Column(sqlite3_context* ctx, int raw_col) {
  uint32_t column = static_cast<uint32_t>(raw_col);
  SqlValue value = iterator_->Get(column);
  switch (value.type) {
    case SqlValue::Type::kLong:
      sqlite3_result_int64(ctx, value.long_value);
      break;
    case SqlValue::Type::kDouble:
      sqlite3_result_double(ctx, value.double_value);
      break;
    case SqlValue::Type::kString: {
      // We can say kSqliteStatic here because all strings are expected to
      // come from the string pool and thus will be valid for the lifetime
      // of trace processor.
      sqlite3_result_text(ctx, value.string_value, -1,
                          sqlite_utils::kSqliteStatic);
      break;
    }
    case SqlValue::Type::kBytes: {
      // We can say kSqliteStatic here because for our iterator will hold
      // onto the pointer as long as we don't call Next() but that only
      // happens with Next() is called on the Cursor itself at which point
      // SQLite no longer cares about the bytes pointer.
      sqlite3_result_blob(ctx, value.bytes_value,
                          static_cast<int>(value.bytes_count),
                          sqlite_utils::kSqliteStatic);
      break;
    }
    case SqlValue::Type::kNull:
      sqlite3_result_null(ctx);
      break;
  }
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
