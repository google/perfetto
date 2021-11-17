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

#include "src/trace_processor/sqlite/create_function.h"

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

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

struct Prototype {
  struct Argument {
    std::string dollar_name;
    SqlValue::Type type;

    bool operator==(const Argument& other) const {
      return dollar_name == other.dollar_name && type == other.type;
    }
  };
  std::string function_name;
  std::vector<Argument> arguments;

  bool operator==(const Prototype& other) const {
    return function_name == other.function_name && arguments == other.arguments;
  }
  bool operator!=(const Prototype& other) const { return !(*this == other); }
};

base::Status ParsePrototype(base::StringView raw, Prototype& out) {
  // Examples of function prototypes:
  // ANDROID_SDK_LEVEL()
  // STARTUP_SLICE(dur_ns INT)
  // FIND_NEXT_SLICE_WITH_NAME(ts INT, name STRING)

  size_t function_name_end = raw.find('(');
  if (function_name_end == base::StringView::npos) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: unable to find bracket starting "
        "argument list",
        raw.ToStdString().c_str());
  }

  base::StringView function_name = raw.substr(0, function_name_end);
  if (!IsValidName(function_name)) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: function name %s is not alphanumeric",
        raw.ToStdString().c_str(), function_name.ToStdString().c_str());
  }

  size_t args_start = function_name_end + 1;
  size_t args_end = raw.find(')', function_name_end);
  if (args_end == base::StringView::npos) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: unable to find bracket ending "
        "argument list",
        raw.ToStdString().c_str());
  }

  base::StringView args_str = raw.substr(args_start, args_end - args_start);
  for (const auto& arg : base::SplitString(args_str.ToStdString(), ",")) {
    const auto& arg_name_and_type = base::SplitString(arg, " ");
    if (arg_name_and_type.size() != 2) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s, arg=%s]: argument in function "
          "prototye should be of the form `name type`",
          raw.ToStdString().c_str(), arg.c_str());
    }

    const auto& arg_name = arg_name_and_type[0];
    const auto& arg_type_str = arg_name_and_type[1];
    if (!IsValidName(base::StringView(arg_name))) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s, arg=%s]: argument is not alphanumeric",
          raw.ToStdString().c_str(), arg.c_str());
    }

    auto opt_arg_type = ParseType(base::StringView(arg_type_str));
    if (!opt_arg_type) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s, arg=%s]: unknown arg type",
          raw.ToStdString().c_str(), arg.c_str());
    }

    SqlValue::Type arg_type = *opt_arg_type;
    PERFETTO_DCHECK(arg_type != SqlValue::Type::kNull);
    out.arguments.push_back({"$" + arg_name, arg_type});
  }

  out.function_name = function_name.ToStdString();
  return base::OkStatus();
}

struct CreatedFunction : public SqlFunction {
  struct Context {
    sqlite3* db;
    Prototype prototype;
    SqlValue::Type return_type;
    std::string sql;
    sqlite3_stmt* stmt;
  };

  static base::Status Run(Context* ctx,
                          size_t argc,
                          sqlite3_value** argv,
                          SqlValue& out,
                          Destructors&);
  static base::Status Cleanup(Context*);
};

base::Status SqliteRetToStatus(CreatedFunction::Context* ctx, int ret) {
  if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
    return base::ErrStatus("%s: SQLite error while executing function body: %s",
                           ctx->prototype.function_name.c_str(),
                           sqlite3_errmsg(ctx->db));
  }
  return base::OkStatus();
}

base::Status CreatedFunction::Run(CreatedFunction::Context* ctx,
                                  size_t argc,
                                  sqlite3_value** argv,
                                  SqlValue& out,
                                  Destructors&) {
  if (argc != ctx->prototype.arguments.size()) {
    return base::ErrStatus(
        "%s: invalid number of args; expected %zu, received %zu",
        ctx->prototype.function_name.c_str(), ctx->prototype.arguments.size(),
        argc);
  }

  // Type check all the arguments.
  for (size_t i = 0; i < argc; ++i) {
    sqlite3_value* arg = argv[i];
    base::Status status =
        TypeCheckSqliteValue(arg, ctx->prototype.arguments[i].type);
    if (!status.ok()) {
      return base::ErrStatus("%s[arg=%s]: argument %zu %s",
                             ctx->prototype.function_name.c_str(),
                             sqlite3_value_text(arg), i, status.c_message());
    }
  }

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < argc; ++i) {
    const auto& arg = ctx->prototype.arguments[i];
    int index =
        sqlite3_bind_parameter_index(ctx->stmt, arg.dollar_name.c_str());

    // If the argument is not in the query, this just means its an unused
    // argument which we can just ignore.
    if (index == 0)
      continue;

    int ret = sqlite3_bind_value(ctx->stmt, index, argv[i]);
    if (ret != SQLITE_OK) {
      return base::ErrStatus(
          "%s: SQLite error while binding value to argument %zu: %s",
          ctx->prototype.function_name.c_str(), i, sqlite3_errmsg(ctx->db));
    }
  }

  int ret = sqlite3_step(ctx->stmt);
  RETURN_IF_ERROR(SqliteRetToStatus(ctx, ret));
  if (ret == SQLITE_DONE)
    // No return value means we just return don't set |out|.
    return base::OkStatus();

  PERFETTO_DCHECK(ret == SQLITE_ROW);
  size_t col_count = static_cast<size_t>(sqlite3_column_count(ctx->stmt));
  if (col_count != 1) {
    return base::ErrStatus(
        "%s: SQL definition should only return one column: returned %zu "
        "columns",
        ctx->prototype.function_name.c_str(), col_count);
  }

  out = sqlite_utils::SqliteValueToSqlValue(sqlite3_column_value(ctx->stmt, 0));
  return base::OkStatus();
}

base::Status CreatedFunction::Cleanup(CreatedFunction::Context* ctx) {
  int ret = sqlite3_step(ctx->stmt);
  RETURN_IF_ERROR(SqliteRetToStatus(ctx, ret));
  if (ret == SQLITE_ROW) {
    return base::ErrStatus(
        "%s: multiple values were returned when executing function body",
        ctx->prototype.function_name.c_str());
  }
  PERFETTO_DCHECK(ret == SQLITE_DONE);

  // Make sure to reset the statement to remove any bindings.
  ret = sqlite3_reset(ctx->stmt);
  if (ret != SQLITE_OK) {
    return base::ErrStatus("%s: error while resetting metric",
                           ctx->prototype.function_name.c_str());
  }
  return base::OkStatus();
}

}  // namespace

size_t CreateFunction::NameAndArgc::Hasher::operator()(
    const NameAndArgc& s) const noexcept {
  base::Hash hash;
  hash.Update(s.name.data(), s.name.size());
  hash.Update(s.argc);
  return static_cast<size_t>(hash.digest());
}

base::Status CreateFunction::Run(CreateFunction::Context* ctx,
                                 size_t argc,
                                 sqlite3_value** argv,
                                 SqlValue&,
                                 Destructors&) {
  if (argc != 3) {
    return base::ErrStatus(
        "CREATE_FUNCTION: invalid number of args; expected %u, received %zu",
        3u, argc);
  }

  sqlite3_value* prototype_value = argv[0];
  sqlite3_value* return_type_value = argv[1];
  sqlite3_value* sql_defn_value = argv[2];

  // Type check all the arguments.
  {
    auto type_check = [prototype_value](sqlite3_value* value,
                                        SqlValue::Type type, const char* desc) {
      base::Status status = TypeCheckSqliteValue(value, type);
      if (!status.ok()) {
        return base::ErrStatus("CREATE_FUNCTION[prototype=%s]: %s %s",
                               sqlite3_value_text(prototype_value), desc,
                               status.c_message());
      }
      return base::OkStatus();
    };

    RETURN_IF_ERROR(type_check(prototype_value, SqlValue::Type::kString,
                               "function name (first argument)"));
    RETURN_IF_ERROR(type_check(return_type_value, SqlValue::Type::kString,
                               "return type (second argument)"));
    RETURN_IF_ERROR(type_check(sql_defn_value, SqlValue::Type::kString,
                               "SQL definition (third argument)"));
  }

  // Extract the arguments from the value wrappers.
  auto extract_string = [](sqlite3_value* value) -> base::StringView {
    return reinterpret_cast<const char*>(sqlite3_value_text(value));
  };
  base::StringView prototype_str = extract_string(prototype_value);
  base::StringView return_type_str = extract_string(return_type_value);
  std::string sql_defn_str = extract_string(sql_defn_value).ToStdString();

  // Parse all the arguments into a more friendly form.
  Prototype prototype;
  RETURN_IF_ERROR(ParsePrototype(prototype_str, prototype));

  // Parse the return type into a enum format.
  auto opt_return_type = ParseType(return_type_str);
  if (!opt_return_type) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s, return=%s]: unknown return type "
        "specified",
        prototype_str.ToStdString().c_str(),
        return_type_str.ToStdString().c_str());
  }
  SqlValue::Type return_type = *opt_return_type;

  int created_argc = static_cast<int>(prototype.arguments.size());
  NameAndArgc key{prototype.function_name, created_argc};
  auto it = ctx->state->find(key);
  if (it != ctx->state->end()) {
    // If the function already exists, just verify that the prototype, return
    // type and SQL matches exactly with what we already had registered. By
    // doing this, we can avoid the problem plaguing C++ macros where macro
    // ordering determines which one gets run.
    auto* created_ctx = static_cast<CreatedFunction::Context*>(
        it->second.created_functon_context);

    if (created_ctx->prototype != prototype) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: function prototype changed",
          prototype_str.ToStdString().c_str());
    }

    if (created_ctx->return_type != return_type) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: return type changed from %s to %s",
          prototype_str.ToStdString().c_str(),
          SqliteTypeToFriendlyString(created_ctx->return_type),
          return_type_str.ToStdString().c_str());
    }

    if (created_ctx->sql != sql_defn_str) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: function SQL changed from %s to %s",
          prototype_str.ToStdString().c_str(), created_ctx->sql.c_str(),
          sql_defn_str.c_str());
    }
    return base::OkStatus();
  }

  // Prepare the SQL definition as a statement using SQLite.
  ScopedStmt stmt;
  sqlite3_stmt* stmt_raw = nullptr;
  int ret = sqlite3_prepare_v2(ctx->db, sql_defn_str.data(),
                               static_cast<int>(sql_defn_str.size()), &stmt_raw,
                               nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: SQLite error when preparing "
        "statement "
        "%s",
        prototype_str.ToStdString().c_str(), sqlite3_errmsg(ctx->db));
  }
  stmt.reset(stmt_raw);

  std::unique_ptr<CreatedFunction::Context> created(
      new CreatedFunction::Context{ctx->db, std::move(prototype), return_type,
                                   std::move(sql_defn_str), stmt.get()});
  CreatedFunction::Context* created_ptr = created.get();
  RETURN_IF_ERROR(RegisterSqlFunction<CreatedFunction>(
      ctx->db, key.name.c_str(), created_argc, std::move(created)));
  ctx->state->emplace(key, PerFunctionState{std::move(stmt), created_ptr});

  // CREATE_FUNCTION doesn't have a return value so just don't sent |out|.
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
