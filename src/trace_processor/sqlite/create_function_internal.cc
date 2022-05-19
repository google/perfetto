/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/sqlite/create_function_internal.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

bool IsValidName(base::StringView str) {
  auto pred = [](char c) { return !(isalnum(c) || c == '_'); };
  return std::find_if(str.begin(), str.end(), pred) == str.end();
}

base::Optional<SqlValue::Type> ParseType(base::StringView str) {
  if (str == "INT" || str == "LONG" || str == "BOOL") {
    return SqlValue::Type::kLong;
  } else if (str == "DOUBLE" || str == "FLOAT") {
    return SqlValue::Type::kDouble;
  } else if (str == "STRING") {
    return SqlValue::Type::kString;
  } else if (str == "PROTO" || str == "BYTES") {
    return SqlValue::Type::kBytes;
  }
  return base::nullopt;
}

const char* SqliteTypeToFriendlyString(SqlValue::Type type) {
  switch (type) {
    case SqlValue::Type::kNull:
      return "NULL";
    case SqlValue::Type::kLong:
      return "INT/LONG/BOOL";
    case SqlValue::Type::kDouble:
      return "FLOAT/DOUBLE";
    case SqlValue::Type::kString:
      return "STRING";
    case SqlValue::Type::kBytes:
      return "BYTES/PROTO";
  }
  PERFETTO_FATAL("For GCC");
}

base::Status TypeCheckSqliteValue(sqlite3_value* value,
                                  SqlValue::Type expected_type) {
  SqlValue::Type actual_type =
      sqlite_utils::SqliteTypeToSqlValueType(sqlite3_value_type(value));
  if (actual_type != SqlValue::Type::kNull && actual_type != expected_type) {
    return base::ErrStatus(
        "does not have expected type: expected %s, actual %s",
        SqliteTypeToFriendlyString(expected_type),
        SqliteTypeToFriendlyString(actual_type));
  }
  return base::OkStatus();
}

base::Status ParseFunctionName(base::StringView raw, base::StringView& out) {
  size_t function_name_end = raw.find('(');
  if (function_name_end == base::StringView::npos)
    return base::ErrStatus("unable to find bracket starting argument list");

  base::StringView function_name = raw.substr(0, function_name_end);
  if (!IsValidName(function_name)) {
    return base::ErrStatus("function name %s is not alphanumeric",
                           function_name.ToStdString().c_str());
  }
  out = function_name;
  return base::OkStatus();
}

base::Status ParseArgs(std::string args,
                       std::vector<Prototype::Argument>& out) {
  for (const auto& arg : base::SplitString(args, ",")) {
    const auto& arg_name_and_type = base::SplitString(arg, " ");
    if (arg_name_and_type.size() != 2) {
      return base::ErrStatus(
          "argument %s in function prototype should be of the form `name type`",
          arg.c_str());
    }

    const auto& arg_name = arg_name_and_type[0];
    const auto& arg_type_str = arg_name_and_type[1];
    if (!IsValidName(base::StringView(arg_name)))
      return base::ErrStatus("argument %s is not alphanumeric", arg.c_str());

    auto opt_arg_type = ParseType(base::StringView(arg_type_str));
    if (!opt_arg_type)
      return base::ErrStatus("unknown argument type in argument %s",
                             arg.c_str());

    SqlValue::Type arg_type = *opt_arg_type;
    PERFETTO_DCHECK(arg_type != SqlValue::Type::kNull);
    out.push_back({arg_name, "$" + arg_name, arg_type});
  }
  return base::OkStatus();
}

base::Status ParsePrototype(base::StringView raw, Prototype& out) {
  // Examples of function prototypes:
  // ANDROID_SDK_LEVEL()
  // STARTUP_SLICE(dur_ns INT)
  // FIND_NEXT_SLICE_WITH_NAME(ts INT, name STRING)

  base::StringView function_name;
  RETURN_IF_ERROR(ParseFunctionName(raw, function_name));

  size_t function_name_end = function_name.size();
  size_t args_start = function_name_end + 1;
  size_t args_end = raw.find(')', args_start);
  if (args_end == base::StringView::npos)
    return base::ErrStatus("unable to find bracket ending argument list");

  base::StringView args_str = raw.substr(args_start, args_end - args_start);
  RETURN_IF_ERROR(ParseArgs(args_str.ToStdString(), out.arguments));

  out.function_name = function_name.ToStdString();
  return base::OkStatus();
}

base::Status SqliteRetToStatus(sqlite3* db,
                               const std::string& function_name,
                               int ret) {
  if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
    return base::ErrStatus("%s: SQLite error while executing function body: %s",
                           function_name.c_str(), sqlite3_errmsg(db));
  }
  return base::OkStatus();
}

base::Status MaybeBindArgument(sqlite3_stmt* stmt,
                               const std::string& function_name,
                               const Prototype::Argument& arg,
                               sqlite3_value* value) {
  int index = sqlite3_bind_parameter_index(stmt, arg.dollar_name.c_str());

  // If the argument is not in the query, this just means its an unused
  // argument which we can just ignore.
  if (index == 0)
    return base::Status();

  int ret = sqlite3_bind_value(stmt, index, value);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "%s: SQLite error while binding value to argument %s: %s",
        function_name.c_str(), arg.name.c_str(),
        sqlite3_errmsg(sqlite3_db_handle(stmt)));
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
