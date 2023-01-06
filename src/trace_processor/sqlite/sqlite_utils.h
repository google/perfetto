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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_UTILS_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_UTILS_H_

#include <math.h>
#include <sqlite3.h>
#include <cstddef>
#include <cstring>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"

namespace perfetto {
namespace trace_processor {
namespace sqlite_utils {

const auto kSqliteStatic = reinterpret_cast<sqlite3_destructor_type>(0);
const auto kSqliteTransient = reinterpret_cast<sqlite3_destructor_type>(-1);

inline bool IsOpEq(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_EQ;
}
inline bool IsOpLe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LE;
}
inline bool IsOpLt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_LT;
}
inline bool IsOpGe(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GE;
}
inline bool IsOpGt(int op) {
  return op == SQLITE_INDEX_CONSTRAINT_GT;
}

inline SqlValue::Type SqliteTypeToSqlValueType(int sqlite_type) {
  switch (sqlite_type) {
    case SQLITE_NULL:
      return SqlValue::Type::kNull;
    case SQLITE_BLOB:
      return SqlValue::Type::kBytes;
    case SQLITE_INTEGER:
      return SqlValue::Type::kLong;
    case SQLITE_FLOAT:
      return SqlValue::Type::kDouble;
    case SQLITE_TEXT:
      return SqlValue::Type::kString;
  }
  PERFETTO_FATAL("Unknown SQLite type %d", sqlite_type);
}

inline SqlValue SqliteValueToSqlValue(sqlite3_value* value) {
  SqlValue sql_value;
  switch (sqlite3_value_type(value)) {
    case SQLITE_INTEGER:
      sql_value.type = SqlValue::Type::kLong;
      sql_value.long_value = sqlite3_value_int64(value);
      break;
    case SQLITE_FLOAT:
      sql_value.type = SqlValue::Type::kDouble;
      sql_value.double_value = sqlite3_value_double(value);
      break;
    case SQLITE_TEXT:
      sql_value.type = SqlValue::Type::kString;
      sql_value.string_value =
          reinterpret_cast<const char*>(sqlite3_value_text(value));
      break;
    case SQLITE_BLOB:
      sql_value.type = SqlValue::Type::kBytes;
      sql_value.bytes_value = sqlite3_value_blob(value);
      sql_value.bytes_count = static_cast<size_t>(sqlite3_value_bytes(value));
      break;
  }
  return sql_value;
}

inline base::Optional<std::string> SqlValueToString(SqlValue value) {
  switch (value.type) {
    case SqlValue::Type::kString:
      return value.AsString();
    case SqlValue::Type::kDouble:
      return std::to_string(value.AsDouble());
    case SqlValue::Type::kLong:
      return std::to_string(value.AsLong());
    case SqlValue::Type::kBytes:
    case SqlValue::Type::kNull:
      return base::nullopt;
  }
  PERFETTO_FATAL("For GCC");
}

inline void ReportSqlValue(
    sqlite3_context* ctx,
    const SqlValue& value,
    sqlite3_destructor_type string_destructor = kSqliteTransient,
    sqlite3_destructor_type bytes_destructor = kSqliteTransient) {
  switch (value.type) {
    case SqlValue::Type::kLong:
      sqlite3_result_int64(ctx, value.long_value);
      break;
    case SqlValue::Type::kDouble:
      sqlite3_result_double(ctx, value.double_value);
      break;
    case SqlValue::Type::kString: {
      sqlite3_result_text(ctx, value.string_value, -1, string_destructor);
      break;
    }
    case SqlValue::Type::kBytes:
      sqlite3_result_blob(ctx, value.bytes_value,
                          static_cast<int>(value.bytes_count),
                          bytes_destructor);
      break;
    case SqlValue::Type::kNull:
      sqlite3_result_null(ctx);
      break;
  }
}

inline ScopedSqliteString ExpandedSqlForStmt(sqlite3_stmt* stmt) {
  return ScopedSqliteString(sqlite3_expanded_sql(stmt));
}

inline base::Status FormatErrorMessage(base::StringView sql,
                                       sqlite3* db,
                                       int error_code) {
  uint32_t offset = static_cast<uint32_t>(sqlite3_error_offset(db));

  auto error_opt = FindLineWithOffset(sql, offset);

  if (!error_opt.has_value()) {
    return base::ErrStatus("Error: %s (errcode: %d)", sqlite3_errmsg(db),
                           error_code);
  }

  auto error = error_opt.value();

  return base::ErrStatus(
      "Error in line:%u, col: %u.\n"
      "%s\n"
      "%s^\n"
      "%s (errcode: %d)",
      error.line_num, error.line_offset + 1, error.line.ToStdString().c_str(),
      std::string(error.line_offset, ' ').c_str(), sqlite3_errmsg(db),
      error_code);
}

inline base::Status FormatErrorMessage(sqlite3_stmt* stmt,
                                       base::Optional<base::StringView> sql,
                                       sqlite3* db,
                                       int error_code) {
  if (stmt) {
    auto expanded_sql = ExpandedSqlForStmt(stmt);
    PERFETTO_CHECK(expanded_sql);
    return FormatErrorMessage(expanded_sql.get(), db, error_code);
  }
  PERFETTO_CHECK(sql.has_value());
  return FormatErrorMessage(sql.value(), db, error_code);
}

inline base::Status PrepareStmt(sqlite3* db,
                                const char* sql,
                                ScopedStmt* stmt,
                                const char** tail) {
  sqlite3_stmt* raw_stmt = nullptr;
  int err = sqlite3_prepare_v2(db, sql, -1, &raw_stmt, tail);
  stmt->reset(raw_stmt);
  if (err != SQLITE_OK)
    return base::ErrStatus("%s", FormatErrorMessage(sql, db, err).c_message());
  return base::OkStatus();
}

inline bool IsStmtDone(sqlite3_stmt* stmt) {
  return !sqlite3_stmt_busy(stmt);
}

inline base::Status StepStmtUntilDone(sqlite3_stmt* stmt) {
  PERFETTO_DCHECK(stmt);

  if (IsStmtDone(stmt))
    return base::OkStatus();

  int err;
  for (err = sqlite3_step(stmt); err == SQLITE_ROW; err = sqlite3_step(stmt)) {
  }
  if (err != SQLITE_DONE) {
    auto db = sqlite3_db_handle(stmt);
    return base::ErrStatus(
        "%s", FormatErrorMessage(stmt, base::nullopt, db, err).c_message());
  }
  return base::OkStatus();
}

// Exracts the given type from the SqlValue if |value| can fit
// in the provided optional. Note that SqlValue::kNull will always
// succeed and cause base::nullopt to be set.
//
// Returns base::ErrStatus if the type does not match or does not
// fit in the width of the provided optional type (i.e. int64 value
// not fitting in int32 optional).
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 base::Optional<int64_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 base::Optional<int32_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 base::Optional<uint32_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 base::Optional<double>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 base::Optional<const char*>&);

// Returns the column names for the table named by |raw_table_name|.
base::Status GetColumnsForTable(sqlite3* db,
                                const std::string& raw_table_name,
                                std::vector<SqliteTable::Column>& columns);

// Reads a `SQLITE_TEXT` value and returns it as a wstring (UTF-16) in the
// default byte order. `value` must be a `SQLITE_TEXT`.
std::wstring SqliteValueToWString(sqlite3_value* value);

// Given an SqlValue::Type, converts it to a human-readable string.
// This should really only be used for debugging messages.
const char* SqliteTypeToFriendlyString(SqlValue::Type type);

// Verifies if |value| has the type represented by |expected_type|.
// Returns base::OkStatus if it does or an base::ErrStatus with an
// appropriate error mesage (incorporating |expected_type_str| if specified).
base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type);
base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type,
                                  const char* expected_type_str);

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_UTILS_H_
