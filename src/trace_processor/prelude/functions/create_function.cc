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
#include "src/trace_processor/sqlite/perfetto_sql_engine.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

base::StatusOr<ScopedStmt> CreateStatement(PerfettoSqlEngine* engine,
                                           const std::string& sql,
                                           const std::string& prototype) {
  ScopedStmt stmt;
  const char* tail = nullptr;
  base::Status status = sqlite_utils::PrepareStmt(engine->sqlite_engine()->db(),
                                                  sql.c_str(), &stmt, &tail);
  if (!status.ok()) {
    return base::ErrStatus(
        "CREATE_FUNCTION[prototype=%s]: SQLite error when preparing "
        "statement %s",
        prototype.c_str(), status.message().c_str());
  }
  return std::move(stmt);
}

struct CreatedFunction : public SqlFunction {
  class Context;

  static base::Status Run(Context* ctx,
                          size_t argc,
                          sqlite3_value** argv,
                          SqlValue& out,
                          Destructors&);
  static base::Status VerifyPostConditions(Context*);
  static void Cleanup(Context*);
};

// This class is used to store the state of a CREATE_FUNCTION call.
// It is used to store the state of the function across multiple invocations
// of the function (e.g. when the function is called recursively).
class CreatedFunction::Context {
 public:
  explicit Context(PerfettoSqlEngine* engine) : engine_(engine) {}

  // Prepare a statement and push it into the stack of allocated statements
  // for this function.
  base::Status PrepareStatement() {
    base::StatusOr<ScopedStmt> stmt =
        CreateStatement(engine_, sql_, prototype_str_);
    RETURN_IF_ERROR(stmt.status());
    is_valid_ = true;
    stmts_.push_back(std::move(stmt.value()));
    return base::OkStatus();
  }

  // Sets the state of the function. Should be called only when the function
  // is invalid (i.e. when it is first created or when the previous statement
  // failed to prepare).
  void Reset(Prototype prototype,
             std::string prototype_str,
             sql_argument::Type return_type,
             std::string sql) {
    // Re-registration of valid functions is not allowed.
    PERFETTO_DCHECK(!is_valid_);
    PERFETTO_DCHECK(stmts_.empty());

    prototype_ = std::move(prototype);
    prototype_str_ = std::move(prototype_str);
    return_type_ = return_type;
    sql_ = std::move(sql);
  }

  // This function is called each time the function is called.
  // It ensures that we have a statement for the current recursion level,
  // allocating a new one if needed.
  base::Status PushStackEntry() {
    ++current_recursion_level_;
    if (current_recursion_level_ > stmts_.size()) {
      return PrepareStatement();
    }
    return base::OkStatus();
  }

  // Returns the statement that is used for the current invocation.
  sqlite3_stmt* CurrentStatement() {
    return stmts_[current_recursion_level_ - 1].get();
  }

  // This function is called each time the function returns and resets the
  // statement that this invocation used.
  void PopStackEntry() {
    if (current_recursion_level_ > stmts_.size()) {
      // This is possible if we didn't prepare the statement and returned
      // an error.
      return;
    }
    sqlite3_reset(CurrentStatement());
    sqlite3_clear_bindings(CurrentStatement());
    --current_recursion_level_;
  }

  PerfettoSqlEngine* engine() const { return engine_; }

  const Prototype& prototype() const { return prototype_; }

  sql_argument::Type return_type() const { return return_type_; }

  const std::string& sql() const { return sql_; }

  bool is_valid() const { return is_valid_; }

 private:
  PerfettoSqlEngine* engine_;
  Prototype prototype_;
  std::string prototype_str_;
  sql_argument::Type return_type_;
  std::string sql_;
  // Perfetto SQL functions support recursion. Given that each function call in
  // the stack requires a dedicated statement, we maintain a stack of prepared
  // statements and use the top one for each new call (allocating a new one if
  // needed).
  std::vector<ScopedStmt> stmts_;
  size_t current_recursion_level_ = 0;
  // Function re-registration is not allowed, but the user is allowed to define
  // the function again if the first call failed. |is_valid_| flag helps that
  // by tracking whether the current function definition is valid (in which case
  // re-registration is not allowed).
  bool is_valid_ = false;
};

base::Status CreatedFunction::Run(CreatedFunction::Context* ctx,
                                  size_t argc,
                                  sqlite3_value** argv,
                                  SqlValue& out,
                                  Destructors&) {
  // Enter the function and ensure that we have a statement allocated.
  RETURN_IF_ERROR(ctx->PushStackEntry());

  if (argc != ctx->prototype().arguments.size()) {
    return base::ErrStatus(
        "%s: invalid number of args; expected %zu, received %zu",
        ctx->prototype().function_name.c_str(),
        ctx->prototype().arguments.size(), argc);
  }

  // Type check all the arguments.
  for (size_t i = 0; i < argc; ++i) {
    sqlite3_value* arg = argv[i];
    sql_argument::Type type = ctx->prototype().arguments[i].type();
    base::Status status = sqlite_utils::TypeCheckSqliteValue(
        arg, sql_argument::TypeToSqlValueType(type),
        sql_argument::TypeToHumanFriendlyString(type));
    if (!status.ok()) {
      return base::ErrStatus("%s[arg=%s]: argument %zu %s",
                             ctx->prototype().function_name.c_str(),
                             sqlite3_value_text(arg), i, status.c_message());
    }
  }

  PERFETTO_TP_TRACE(
      metatrace::Category::FUNCTION, "CREATE_FUNCTION",
      [ctx, argv](metatrace::Record* r) {
        r->AddArg("Function", ctx->prototype().function_name.c_str());
        for (uint32_t i = 0; i < ctx->prototype().arguments.size(); ++i) {
          std::string key = "Arg " + std::to_string(i);
          const char* value =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
          r->AddArg(base::StringView(key),
                    value ? base::StringView(value) : base::StringView("NULL"));
        }
      });

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < argc; ++i) {
    RETURN_IF_ERROR(MaybeBindArgument(ctx->CurrentStatement(),
                                      ctx->prototype().function_name,
                                      ctx->prototype().arguments[i], argv[i]));
  }

  int ret = sqlite3_step(ctx->CurrentStatement());
  RETURN_IF_ERROR(SqliteRetToStatus(ctx->engine()->sqlite_engine()->db(),
                                    ctx->prototype().function_name, ret));
  if (ret == SQLITE_DONE) {
    // No return value means we just return don't set |out|.
    return base::OkStatus();
  }

  PERFETTO_DCHECK(ret == SQLITE_ROW);
  size_t col_count =
      static_cast<size_t>(sqlite3_column_count(ctx->CurrentStatement()));
  if (col_count != 1) {
    return base::ErrStatus(
        "%s: SQL definition should only return one column: returned %zu "
        "columns",
        ctx->prototype().function_name.c_str(), col_count);
  }
  out = sqlite_utils::SqliteValueToSqlValue(
      sqlite3_column_value(ctx->CurrentStatement(), 0));

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
  int ret = sqlite3_step(ctx->CurrentStatement());
  RETURN_IF_ERROR(SqliteRetToStatus(ctx->engine()->sqlite_engine()->db(),
                                    ctx->prototype().function_name, ret));
  if (ret == SQLITE_ROW) {
    auto expanded_sql =
        sqlite_utils::ExpandedSqlForStmt(ctx->CurrentStatement());
    return base::ErrStatus(
        "%s: multiple values were returned when executing function body. "
        "Executed SQL was %s",
        ctx->prototype().function_name.c_str(), expanded_sql.get());
  }
  PERFETTO_DCHECK(ret == SQLITE_DONE);
  return base::OkStatus();
}

void CreatedFunction::Cleanup(CreatedFunction::Context* ctx) {
  // Clear the statement.
  ctx->PopStackEntry();
}

}  // namespace

base::Status CreateFunction::Run(PerfettoSqlEngine* engine,
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

  std::string function_name = prototype.function_name;
  int created_argc = static_cast<int>(prototype.arguments.size());
  auto* ctx = static_cast<CreatedFunction::Context*>(
      engine->sqlite_engine()->GetFunctionContext(prototype.function_name,
                                                  created_argc));
  if (!ctx) {
    // We register the function with SQLite before we prepare the statement so
    // the statement can reference the function itself, enabling recursive
    // calls.
    std::unique_ptr<CreatedFunction::Context> created_fn_ctx =
        std::make_unique<CreatedFunction::Context>(engine);
    ctx = created_fn_ctx.get();
    RETURN_IF_ERROR(engine->RegisterSqlFunction<CreatedFunction>(
        function_name.c_str(), created_argc, std::move(created_fn_ctx)));
  }
  if (ctx->is_valid()) {
    // If the function already exists, just verify that the prototype, return
    // type and SQL matches exactly with what we already had registered. By
    // doing this, we can avoid the problem plaguing C++ macros where macro
    // ordering determines which one gets run.
    if (ctx->prototype() != prototype) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: function prototype changed",
          prototype_str.ToStdString().c_str());
    }

    if (ctx->return_type() != *opt_return_type) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: return type changed from %s to %s",
          prototype_str.ToStdString().c_str(),
          sql_argument::TypeToHumanFriendlyString(ctx->return_type()),
          return_type_str.ToStdString().c_str());
    }

    if (ctx->sql() != sql_defn_str) {
      return base::ErrStatus(
          "CREATE_FUNCTION[prototype=%s]: function SQL changed from %s to %s",
          prototype_str.ToStdString().c_str(), ctx->sql().c_str(),
          sql_defn_str.c_str());
    }

    return base::OkStatus();
  }

  ctx->Reset(std::move(prototype), prototype_str.ToStdString(),
             *opt_return_type, std::move(sql_defn_str));

  // Ideally, we would unregister the function here if the statement prep
  // failed, but SQLite doesn't allow unregistering functions inside active
  // statements. So instead we'll just try to prepare the statement when calling
  // this function, which will return an error.
  return ctx->PrepareStatement();
}

}  // namespace trace_processor
}  // namespace perfetto
