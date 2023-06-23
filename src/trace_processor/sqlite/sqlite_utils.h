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
#include <bitset>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
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

inline std::optional<std::string> SqlValueToString(SqlValue value) {
  switch (value.type) {
    case SqlValue::Type::kString:
      return value.AsString();
    case SqlValue::Type::kDouble:
      return std::to_string(value.AsDouble());
    case SqlValue::Type::kLong:
      return std::to_string(value.AsLong());
    case SqlValue::Type::kBytes:
    case SqlValue::Type::kNull:
      return std::nullopt;
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

inline void SetSqliteError(sqlite3_context* ctx, const base::Status& status) {
  PERFETTO_CHECK(!status.ok());
  sqlite3_result_error(ctx, status.c_message(), -1);
}

inline void SetSqliteError(sqlite3_context* ctx,
                           const std::string& function_name,
                           const base::Status& status) {
  SetSqliteError(ctx, base::ErrStatus("%s: %s", function_name.c_str(),
                                      status.c_message()));
}

// Exracts the given type from the SqlValue if |value| can fit
// in the provided optional. Note that SqlValue::kNull will always
// succeed and cause std::nullopt to be set.
//
// Returns base::ErrStatus if the type does not match or does not
// fit in the width of the provided optional type (i.e. int64 value
// not fitting in int32 optional).
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 std::optional<int64_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 std::optional<int32_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 std::optional<uint32_t>&);
base::Status ExtractFromSqlValue(const SqlValue& value, std::optional<double>&);
base::Status ExtractFromSqlValue(const SqlValue& value,
                                 std::optional<const char*>&);

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

// Verifies if |argc| matches |expected_argc| and returns an appropriate error
// message if they don't match.
base::Status CheckArgCount(const char* function_name,
                           size_t argc,
                           size_t expected_argc);

// Type-safe helpers to extract an arg value from a sqlite3_value*, returning an
// appropriate message if it fails.
base::StatusOr<int64_t> ExtractIntArg(const char* function_name,
                                      const char* arg_name,
                                      sqlite3_value* value);
base::StatusOr<double> ExtractDoubleArg(const char* function_name,
                                        const char* arg_name,
                                        sqlite3_value* value);
base::StatusOr<std::string> ExtractStringArg(const char* function_name,
                                             const char* arg_name,
                                             sqlite3_value* value);

// Verifies if |value| has the type represented by |expected_type|.
// Returns base::OkStatus if it does or an base::ErrStatus with an
// appropriate error mesage (incorporating |expected_type_str| if specified).
base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type);
base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type,
                                  const char* expected_type_str);

namespace internal {

static_assert(sizeof(size_t) * 8 > SqlValue::kLastType);
using ExpectedTypesSet = std::bitset<SqlValue::kLastType + 1>;

template <typename... args>
constexpr ExpectedTypesSet ToExpectedTypesSet(args... expected_type_args) {
  ExpectedTypesSet set;
  for (const SqlValue::Type t : {expected_type_args...}) {
    set.set(static_cast<size_t>(t));
  }
  return set;
}

base::StatusOr<SqlValue> ExtractArgument(size_t argc,
                                         sqlite3_value** argv,
                                         const char* argument_name,
                                         size_t arg_index,
                                         ExpectedTypesSet expected_types);
base::Status InvalidArgumentTypeError(const char* argument_name,
                                      size_t arg_index,
                                      SqlValue::Type actual_type,
                                      ExpectedTypesSet expected_types);
}  // namespace internal

template <typename... args>
base::Status InvalidArgumentTypeError(const char* argument_name,
                                      size_t arg_index,
                                      SqlValue::Type actual_type,
                                      SqlValue::Type expected_type,
                                      args... expected_type_args) {
  return internal::InvalidArgumentTypeError(
      argument_name, arg_index, actual_type,
      internal::ToExpectedTypesSet(expected_type, expected_type_args...));
}

base::Status MissingArgumentError(const char* argument_name);

base::Status ToInvalidArgumentError(const char* argument_name,
                                    size_t arg_index,
                                    const base::Status error);

template <typename... args>
base::StatusOr<SqlValue> ExtractArgument(size_t argc,
                                         sqlite3_value** argv,
                                         const char* argument_name,
                                         size_t arg_index,
                                         SqlValue::Type expected_type,
                                         args... expected_type_args) {
  return internal::ExtractArgument(
      argc, argv, argument_name, arg_index,
      internal::ToExpectedTypesSet(expected_type, expected_type_args...));
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_UTILS_H_
