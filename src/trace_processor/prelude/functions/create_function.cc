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

#include "src/trace_processor/prelude/functions/create_function.h"

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/prelude/functions/create_function_internal.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

struct CreatedFunction : public SqlFunction {
  struct Context {
    SqliteEngine* engine;
    Prototype prototype;
    sql_argument::Type return_type;
    std::string sql;
    ScopedStmt stmt;
  };

  static base::Status Run(Context* ctx,
                          size_t argc,
                          sqlite3_value** argv,
                          SqlValue& out,
                          Destructors&);
  static base::Status VerifyPostConditions(Context*);
  static void Cleanup(Context*);
};

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
    sql_argument::Type type = ctx->prototype.arguments[i].type();
    base::Status status = sqlite_utils::TypeCheckSqliteValue(
        arg, sql_argument::TypeToSqlValueType(type),
        sql_argument::TypeToHumanFriendlyString(type));
    if (!status.ok()) {
      return base::ErrStatus("%s[arg=%s]: argument %zu %s",
                             ctx->prototype.function_name.c_str(),
                             sqlite3_value_text(arg), i, status.c_message());
    }
  }

  PERFETTO_TP_TRACE(
      metatrace::Category::FUNCTION, "CREATE_FUNCTION",
      [ctx, argv](metatrace::Record* r) {
        r->AddArg("Function", ctx->prototype.function_name.c_str());
        for (uint32_t i = 0; i < ctx->prototype.arguments.size(); ++i) {
          std::string key = "Arg " + std::to_string(i);
          const char* value =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
          r->AddArg(base::StringView(key),
                    value ? base::StringView(value) : base::StringView("NULL"));
        }
      });

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < argc; ++i) {
    RETURN_IF_ERROR(MaybeBindArgument(ctx->stmt.get(),
                                      ctx->prototype.function_name,
                                      ctx->prototype.arguments[i], argv[i]));
  }

  int ret = sqlite3_step(ctx->stmt.get());
  RETURN_IF_ERROR(
      SqliteRetToStatus(ctx->engine->db(), ctx->prototype.function_name, ret));
  if (ret == SQLITE_DONE) {
    // No return value means we just return don't set |out|.
    return base::OkStatus();
  }

  PERFETTO_DCHECK(ret == SQLITE_ROW);
  size_t col_count = static_cast<size_t>(sqlite3_column_count(ctx->stmt.get()));
  if (col_count != 1) {
    return base::ErrStatus(
        "%s: SQL definition should only return one column: returned %zu "
        "columns",
        ctx->prototype.function_name.c_str(), col_count);
  }

  out = sqlite_utils::SqliteValueToSqlValue(
      sqlite3_column_value(ctx->stmt.get(), 0));

  // If we return a bytes type but have a null pointer, SQLite will convert this
  // to an SQL null. However, for proto build functions, we actively want to
  // distinguish between nulls and 0 byte strings. Therefore, change the value
  // to an empty string.
  if (out.type == SqlValue::kBytes && out.bytes_value == nullptr) {
    PERFETTO_DCHECK(out.bytes_count == 0);
    out.bytes_value = "";
  }
  return base::OkStatus();
}

base::Status CreatedFunction::VerifyPostConditions(Context* ctx) {
  int ret = sqlite3_step(ctx->stmt.get());
  RETURN_IF_ERROR(
      SqliteRetToStatus(ctx->engine->db(), ctx->prototype.function_name, ret));
  if (ret == SQLITE_ROW) {
    auto expanded_sql = sqlite_utils::ExpandedSqlForStmt(ctx->stmt.get());
    return base::ErrStatus(
        "%s: multiple values were returned when executing function body. "
        "Executed SQL was %s",
        ctx->prototype.function_name.c_str(), expanded_sql.get());
  }
  PERFETTO_DCHECK(ret == SQLITE_DONE);
  return base::OkStatus();
}

void CreatedFunction::Cleanup(CreatedFunction::Context* ctx) {
  sqlite3_reset(ctx->stmt.get());
  sqlite3_clear_bindings(ctx->stmt.get());
}

}  // namespace

base::Status CreateFunction::Run(SqliteEngine* engine,
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
      base::Status status = sqlite_utils::TypeCheckSqliteValue(value, type);
      if (!status.ok()) {
        return base::ErrStatus("CREATE_FUNCTION[prototype=%s]: %s %s",
                               sqlite3_value_text(prototype_value), desc,
                               status.c_message());
      }
      return base::OkStatus();
    };

    RETURN_IF_ERROR(type_check(prototype_value, SqlValue::Type::kString,
                               "function prototype (first argument)"));
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
  base::Status status = ParsePrototype(prototype_str, prototype);
  if (!status.ok()) {
    return base::ErrStatus("CREATE_FUNCTION[prototype=%s]: %s",
                           prototype_str.ToStdString().c_str(),
                           status.c_message());
  }

  // Parse the return type into a enum format.
  auto opt_return_type = sql_argument::ParseType(return_type_str);
  if (!opt_return_type) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s, return=%s]: unknown return type "
        "specified",
        prototype_str.ToStdString().c_str(),
        return_type_str.ToStdString().c_str());
  }

  int created_argc = static_cast<int>(prototype.arguments.size());
  auto* fn_ctx =
      engine->GetFunctionContext(prototype.function_name, created_argc);
  if (fn_ctx) {
    // If the function already exists, just verify that the prototype, return
    // type and SQL matches exactly with what we already had registered. By
    // doing this, we can avoid the problem plaguing C++ macros where macro
    // ordering determines which one gets run.
    auto* created_ctx = static_cast<CreatedFunction::Context*>(fn_ctx);
    if (created_ctx->prototype != prototype) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: function prototype changed",
          prototype_str.ToStdString().c_str());
    }

    if (created_ctx->return_type != *opt_return_type) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: return type changed from %s to %s",
          prototype_str.ToStdString().c_str(),
          sql_argument::TypeToHumanFriendlyString(created_ctx->return_type),
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
  int ret = sqlite3_prepare_v2(engine->db(), sql_defn_str.data(),
                               static_cast<int>(sql_defn_str.size()), &stmt_raw,
                               nullptr);
  if (ret != SQLITE_OK) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: SQLite error when preparing "
        "statement %s",
        prototype_str.ToStdString().c_str(),
        sqlite_utils::FormatErrorMessage(
            stmt_raw, base::StringView(sql_defn_str), engine->db(), ret)
            .c_message());
  }
  stmt.reset(stmt_raw);

  std::string function_name = prototype.function_name;
  std::unique_ptr<CreatedFunction::Context> created_fn_ctx(
      new CreatedFunction::Context{engine, std::move(prototype),
                                   *opt_return_type, std::move(sql_defn_str),
                                   std::move(stmt)});
  RETURN_IF_ERROR(engine->RegisterSqlFunction<CreatedFunction>(
      function_name.c_str(), created_argc, std::move(created_fn_ctx)));

  // CREATE_FUNCTION doesn't have a return value so just don't sent |out|.
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
