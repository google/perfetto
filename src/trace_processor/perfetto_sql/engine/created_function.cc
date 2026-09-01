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

#include "src/trace_processor/perfetto_sql/engine/created_function.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

namespace {

void ReturnSqlValue(sqlite3_context* ctx, const SqlValue& value) {
  switch (value.type) {
    case SqlValue::Type::kNull:
      sqlite::utils::ReturnNullFromFunction(ctx);
      break;
    case SqlValue::Type::kLong:
      sqlite::result::Long(ctx, value.long_value);
      break;
    case SqlValue::Type::kDouble:
      sqlite::result::Double(ctx, value.double_value);
      break;
    case SqlValue::Type::kString:
      sqlite::result::RawString(ctx, value.string_value, -1,
                                sqlite::result::kSqliteTransient);
      break;
    case SqlValue::Type::kBytes:
      sqlite::result::RawBytes(ctx, value.bytes_value,
                               static_cast<int>(value.bytes_count),
                               sqlite::result::kSqliteTransient);
      break;
  }
}

base::Status CheckNoMoreRows(sqlite3_stmt* stmt,
                             sqlite3* db,
                             const FunctionPrototype& prototype) {
  int ret = sqlite3_step(stmt);
  RETURN_IF_ERROR(SqliteRetToStatus(db, prototype.function_name, ret));
  if (ret == SQLITE_ROW) {
    auto expanded_sql = ScopedSqliteString(sqlite3_expanded_sql(stmt));
    return base::ErrStatus(
        "%s: multiple values were returned when executing function body. "
        "Executed SQL was %s",
        prototype.function_name.c_str(), expanded_sql.get());
  }
  PERFETTO_DCHECK(ret == SQLITE_DONE);
  return base::OkStatus();
}

// Note: if the returned type is string / bytes, it will be invalidated by the
// next call to SQLite, so the caller must take care to either copy or use the
// value before calling SQLite again.
base::StatusOr<SqlValue> EvaluateScalarStatement(
    sqlite3_stmt* stmt,
    sqlite3* db,
    const FunctionPrototype& prototype) {
  int ret = sqlite3_step(stmt);
  RETURN_IF_ERROR(SqliteRetToStatus(db, prototype.function_name, ret));
  if (ret == SQLITE_DONE) {
    // No return value means we just return don't set |out|.
    return SqlValue();
  }

  PERFETTO_DCHECK(ret == SQLITE_ROW);
  size_t col_count = static_cast<size_t>(sqlite3_column_count(stmt));
  if (col_count != 1) {
    return base::ErrStatus(
        "%s: SQL definition should only return one column: returned %zu "
        "columns",
        prototype.function_name.c_str(), col_count);
  }

  SqlValue result =
      sqlite::utils::SqliteValueToSqlValue(sqlite3_column_value(stmt, 0));

  // If we return a bytes type but have a null pointer, SQLite will convert this
  // to an SQL null. However, for proto build functions, we actively want to
  // distinguish between nulls and 0 byte strings. Therefore, change the value
  // to an empty string.
  if (result.type == SqlValue::kBytes && result.bytes_value == nullptr) {
    PERFETTO_DCHECK(result.bytes_count == 0);
    result.bytes_value = "";
  }

  return result;
}

base::Status BindArguments(sqlite3_stmt* stmt,
                           const FunctionPrototype& prototype,
                           size_t argc,
                           sqlite3_value** argv) {
  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < argc; ++i) {
    RETURN_IF_ERROR(MaybeBindArgument(stmt, prototype.function_name,
                                      prototype.arguments[i], argv[i]));
  }
  return base::OkStatus();
}

}  // namespace

// This class is used to store the state of a CREATE_FUNCTION call.
// It is used to store the state of the function across multiple invocations
// of the function (e.g. when the function is called recursively).
class State : public CreatedFunction::UserData {
 public:
  explicit State(PerfettoSqlConnection* connection) : engine_(connection) {}
  ~State() override;

  base::Status PrepareStatement() {
    SqliteConnection::PreparedStatement stmt =
        engine_->sqlite_connection()->PrepareStatement(*sql_);
    RETURN_IF_ERROR(stmt.status());
    stmt_ = std::move(stmt);
    is_valid_ = true;
    return base::OkStatus();
  }

  void Reset(FunctionPrototype prototype, SqlSource sql) {
    PERFETTO_DCHECK(!is_valid_);
    PERFETTO_DCHECK(!stmt_);

    prototype_ = std::move(prototype);
    sql_ = std::move(sql);
  }

  base::Status BeginExecution() {
    if (is_executing_) {
      return base::ErrStatus("%s: recursive calls are not supported",
                             prototype_.function_name.c_str());
    }
    if (!stmt_) {
      RETURN_IF_ERROR(PrepareStatement());
    }
    is_executing_ = true;
    return base::OkStatus();
  }

  void EndExecution() {
    PERFETTO_CHECK(is_executing_);
    sqlite3_reset(CurrentStatement());
    sqlite3_clear_bindings(CurrentStatement());
    is_executing_ = false;
  }

  sqlite3_stmt* CurrentStatement() {
    PERFETTO_DCHECK(stmt_);
    return stmt_->sqlite_stmt();
  }

  PerfettoSqlConnection* connection() const { return engine_; }

  const FunctionPrototype& prototype() const { return prototype_; }

  bool is_valid() const { return is_valid_; }

  bool is_executing() const { return is_executing_; }

 private:
  PerfettoSqlConnection* engine_;
  FunctionPrototype prototype_;
  std::optional<SqlSource> sql_;
  std::optional<SqliteConnection::PreparedStatement> stmt_;
  bool is_valid_ = false;
  bool is_executing_ = false;
};

State::~State() = default;

std::unique_ptr<CreatedFunction::UserData> CreatedFunction::MakeContext(
    PerfettoSqlConnection* connection) {
  return std::make_unique<State>(connection);
}

bool CreatedFunction::IsValid(UserData* ctx) {
  return static_cast<State*>(ctx)->is_valid();
}

bool CreatedFunction::IsExecuting(UserData* ctx) {
  return static_cast<State*>(ctx)->is_executing();
}

void CreatedFunction::Reset(UserData* ctx, PerfettoSqlConnection* connection) {
  PERFETTO_CHECK(!IsExecuting(ctx));
  ctx->~UserData();
  new (ctx) State(connection);
}

void CreatedFunction::Step(sqlite3_context* ctx,
                           int argc,
                           sqlite3_value** argv) {
  auto* state = static_cast<State*>(CreatedFunction::GetUserData(ctx));

  if (auto status = state->BeginExecution(); !status.ok()) {
    return sqlite::utils::SetError(ctx, status.c_message());
  }
  struct ScopedCleanup {
    State* state;
    ~ScopedCleanup() { state->EndExecution(); }
  };
  ScopedCleanup scoped_cleanup{state};

  size_t expected_argc = state->prototype().arguments.size();
  if (static_cast<size_t>(argc) != expected_argc) {
    return sqlite::utils::SetError(
        ctx, base::ErrStatus(
                 "%s: invalid number of args; expected %zu, received %d",
                 state->prototype().function_name.c_str(), expected_argc, argc)
                 .c_message());
  }

  for (size_t i = 0; i < expected_argc; ++i) {
    sqlite3_value* arg = argv[i];
    sql_argument::Type type = state->prototype().arguments[i].type();
    if (type == sql_argument::Type::kAny) {
      continue;
    }
    base::Status status = sqlite::utils::TypeCheckSqliteValue(
        arg, sql_argument::TypeToSqlValueType(type),
        sql_argument::TypeToHumanFriendlyString(type));
    if (!status.ok()) {
      return sqlite::utils::SetError(
          ctx, base::ErrStatus("%s[arg=%s]: argument %zu %s",
                               state->prototype().function_name.c_str(),
                               sqlite3_value_text(arg), i, status.c_message())
                   .c_message());
    }
  }

  PERFETTO_TP_TRACE(
      metatrace::Category::FUNCTION_CALL, "SQL_FUNCTION_CALL",
      [state, argv](metatrace::Record* r) {
        r->AddArg("Function", state->prototype().function_name.c_str());
        for (uint32_t i = 0; i < state->prototype().arguments.size(); ++i) {
          std::string key = "Arg " + std::to_string(i);
          const char* value =
              reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
          r->AddArg(base::StringView(key),
                    value ? base::StringView(value) : base::StringView("NULL"));
        }
      });

  if (auto status = BindArguments(state->CurrentStatement(), state->prototype(),
                                  size_t(argc), argv);
      !status.ok()) {
    return sqlite::utils::SetError(ctx, status.c_message());
  }

  auto result = EvaluateScalarStatement(
      state->CurrentStatement(), state->connection()->sqlite_connection()->db(),
      state->prototype());
  if (!result.ok()) {
    return sqlite::utils::SetError(ctx, result.status().c_message());
  }

  ReturnSqlValue(ctx, result.value());
  if (auto status = CheckNoMoreRows(
          state->CurrentStatement(),
          state->connection()->sqlite_connection()->db(), state->prototype());
      !status.ok()) {
    sqlite::utils::SetError(ctx, status.c_message());
  }
}

base::Status CreatedFunction::Prepare(CreatedFunction::UserData* ctx,
                                      FunctionPrototype prototype,
                                      SqlSource source) {
  State* state = static_cast<State*>(ctx);
  state->Reset(std::move(prototype), std::move(source));

  // Ideally, we would unregister the function here if the statement prep
  // failed, but SQLite doesn't allow unregistering functions inside active
  // statements. So instead we'll just try to prepare the statement when calling
  // this function, which will return an error.
  return state->PrepareStatement();
}

}  // namespace perfetto::trace_processor
