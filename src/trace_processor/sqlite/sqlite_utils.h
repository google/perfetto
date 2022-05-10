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

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
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

inline base::Status PrepareStmt(sqlite3* db,
                                const char* sql,
                                ScopedStmt* stmt,
                                const char** tail) {
  sqlite3_stmt* raw_stmt = nullptr;
  int err = sqlite3_prepare_v2(db, sql, -1, &raw_stmt, tail);
  stmt->reset(raw_stmt);
  if (err != SQLITE_OK)
    return base::ErrStatus("%s (errcode: %d)", sqlite3_errmsg(db), err);
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
    return base::ErrStatus("%s (errcode: %d)",
                           sqlite3_errmsg(sqlite3_db_handle(stmt)), err);
  }
  return base::OkStatus();
}

inline base::Status GetColumnsForTable(
    sqlite3* db,
    const std::string& raw_table_name,
    std::vector<SqliteTable::Column>& columns) {
  PERFETTO_DCHECK(columns.empty());
  char sql[1024];
  const char kRawSql[] = "SELECT name, type from pragma_table_info(\"%s\")";

  // Support names which are table valued functions with arguments.
  std::string table_name = raw_table_name.substr(0, raw_table_name.find('('));
  size_t n = base::SprintfTrunc(sql, sizeof(sql), kRawSql, table_name.c_str());
  PERFETTO_DCHECK(n > 0);

  sqlite3_stmt* raw_stmt = nullptr;
  int err =
      sqlite3_prepare_v2(db, sql, static_cast<int>(n), &raw_stmt, nullptr);
  if (err != SQLITE_OK) {
    return base::ErrStatus("Preparing database failed");
  }
  ScopedStmt stmt(raw_stmt);
  PERFETTO_DCHECK(sqlite3_column_count(*stmt) == 2);

  for (;;) {
    err = sqlite3_step(raw_stmt);
    if (err == SQLITE_DONE)
      break;
    if (err != SQLITE_ROW) {
      return base::ErrStatus("Querying schema of table %s failed",
                             raw_table_name.c_str());
    }

    const char* name =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 0));
    const char* raw_type =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 1));
    if (!name || !raw_type || !*name) {
      return base::ErrStatus("Schema for %s has invalid column values",
                             raw_table_name.c_str());
    }

    SqlValue::Type type;
    if (base::CaseInsensitiveEqual(raw_type, "STRING") ||
        base::CaseInsensitiveEqual(raw_type, "TEXT")) {
      type = SqlValue::Type::kString;
    } else if (base::CaseInsensitiveEqual(raw_type, "DOUBLE")) {
      type = SqlValue::Type::kDouble;
    } else if (base::CaseInsensitiveEqual(raw_type, "BIG INT") ||
               base::CaseInsensitiveEqual(raw_type, "UNSIGNED INT") ||
               base::CaseInsensitiveEqual(raw_type, "INT") ||
               base::CaseInsensitiveEqual(raw_type, "BOOLEAN") ||
               base::CaseInsensitiveEqual(raw_type, "INTEGER")) {
      type = SqlValue::Type::kLong;
    } else if (!*raw_type) {
      PERFETTO_DLOG("Unknown column type for %s %s", raw_table_name.c_str(),
                    name);
      type = SqlValue::Type::kNull;
    } else {
      return base::ErrStatus("Unknown column type '%s' on table %s", raw_type,
                             raw_table_name.c_str());
    }
    columns.emplace_back(columns.size(), name, type);
  }
  return base::OkStatus();
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_UTILS_H_
