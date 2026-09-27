/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/sqlite/sqlite_utils.h"

#include <sqlite3.h>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/scoped_db.h"

namespace perfetto::trace_processor::sqlite::utils {
namespace internal {
namespace {
std::string ToExpectedTypesString(ExpectedTypesSet expected_types) {
  PERFETTO_CHECK(expected_types.any());
  std::vector<std::string> type_names;
  for (size_t i = 0; i < expected_types.size(); ++i) {
    if (expected_types[i]) {
      type_names.emplace_back(
          SqliteTypeToFriendlyString(static_cast<SqlValue::Type>(i)));
    }
  }
  std::string result;
  if (type_names.size() > 1) {
    result = "any of ";
  }
  result += base::Join(type_names, ", ");
  return result;
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

}  // namespace

base::Status InvalidArgumentTypeError(const char* argument_name,
                                      size_t arg_index,
                                      SqlValue::Type actual_type,
                                      ExpectedTypesSet expected_types) {
  return ToInvalidArgumentError(
      argument_name, arg_index,
      base::ErrStatus("does not have expected type. Expected %s but found %s",
                      ToExpectedTypesString(expected_types).c_str(),
                      SqliteTypeToFriendlyString(actual_type)));
}

base::StatusOr<SqlValue> ExtractArgument(size_t argc,
                                         sqlite3_value** argv,
                                         const char* argument_name,
                                         size_t arg_index,
                                         ExpectedTypesSet expected_types) {
  if (arg_index >= argc) {
    return MissingArgumentError(argument_name);
  }

  SqlValue value = sqlite::utils::SqliteValueToSqlValue(argv[arg_index]);
  if (!expected_types.test(value.type)) {
    return InvalidArgumentTypeError(argument_name, arg_index, value.type,
                                    expected_types);
  }
  return value;
}
}  // namespace internal

std::vector<SqliteColumn> GetColumns(sqlite3* db, const std::string& name) {
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT name, type, hidden FROM pragma_table_xinfo(?)",
                         -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return {};
  }
  ScopedStmt stmt(raw_stmt);
  sqlite3_bind_text(*stmt, 1, name.c_str(), static_cast<int>(name.size()),
                    kSqliteStatic);
  std::vector<SqliteColumn> columns;
  int rc;
  while ((rc = sqlite3_step(*stmt)) == SQLITE_ROW) {
    const auto* column =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 0));
    const auto* type =
        reinterpret_cast<const char*>(sqlite3_column_text(*stmt, 1));
    // 1 is a hidden column; 2 and 3 are generated ones, which `*` includes.
    columns.push_back({column ? column : "", type ? type : "",
                       sqlite3_column_int(*stmt, 2) == 1});
  }
  return rc == SQLITE_DONE ? columns : std::vector<SqliteColumn>();
}

const char* SqliteTypeToFriendlyString(SqlValue::Type type) {
  switch (type) {
    case SqlValue::Type::kNull:
      return "NULL";
    case SqlValue::Type::kLong:
      return "BOOL/INT/UINT/LONG";
    case SqlValue::Type::kDouble:
      return "FLOAT/DOUBLE";
    case SqlValue::Type::kString:
      return "STRING";
    case SqlValue::Type::kBytes:
      return "BYTES/PROTO";
  }
  PERFETTO_FATAL("For GCC");
}

base::Status CheckArgCount(const char* function_name,
                           size_t argc,
                           size_t expected_argc) {
  if (argc == expected_argc) {
    return base::OkStatus();
  }
  return base::ErrStatus("%s: expected %zu arguments, got %zu", function_name,
                         expected_argc, argc);
}

base::StatusOr<std::string> ExtractStringArg(const char* function_name,
                                             const char* arg_name,
                                             sqlite3_value* sql_value) {
  SqlValue value = SqliteValueToSqlValue(sql_value);
  if (value.type != SqlValue::kString) {
    return base::ErrStatus(
        "%s(%s): value has type %s which does not match the expected type %s",
        function_name, arg_name, SqliteTypeToFriendlyString(value.type),
        SqliteTypeToFriendlyString(SqlValue::kString));
  }
  const char* result = value.AsString();
  PERFETTO_CHECK(result);
  return std::string(result);
}

base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type) {
  return TypeCheckSqliteValue(value, expected_type,
                              SqliteTypeToFriendlyString(expected_type));
}

base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type,
                                  const char* expected_type_str) {
  SqlValue::Type actual_type =
      internal::SqliteTypeToSqlValueType(sqlite3_value_type(value));
  if (actual_type != SqlValue::Type::kNull && actual_type != expected_type) {
    return base::ErrStatus(
        "does not have expected type: expected %s, actual %s",
        expected_type_str, SqliteTypeToFriendlyString(actual_type));
  }
  return base::OkStatus();
}

base::Status MissingArgumentError(const char* argument_name) {
  return base::ErrStatus("argument missing: %s", argument_name);
}

base::Status ToInvalidArgumentError(const char* argument_name,
                                    size_t arg_index,
                                    const base::Status& error) {
  return base::ErrStatus("argument %s at pos %zu: %s", argument_name,
                         arg_index + 1, error.message().c_str());
}

}  // namespace perfetto::trace_processor::sqlite::utils
