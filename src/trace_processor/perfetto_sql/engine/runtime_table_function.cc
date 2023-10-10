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

#include "src/trace_processor/perfetto_sql/engine/runtime_table_function.h"

#include <optional>
#include <utility>

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

void ResetStatement(sqlite3_stmt* stmt) {
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
}

}  // namespace

RuntimeTableFunction::RuntimeTableFunction(sqlite3*, PerfettoSqlEngine* engine)
    : engine_(engine) {}

RuntimeTableFunction::~RuntimeTableFunction() {
  engine_->OnRuntimeTableFunctionDestroyed(name());
}

base::Status RuntimeTableFunction::Init(int,
                                        const char* const*,
                                        Schema* schema) {
  state_ = engine_->GetRuntimeTableFunctionState(name());

  // Now we've parsed prototype and return values, create the schema.
  *schema = CreateSchema();
  return base::OkStatus();
}

SqliteTable::Schema RuntimeTableFunction::CreateSchema() {
  std::vector<Column> columns;
  for (size_t i = 0; i < state_->return_values.size(); ++i) {
    const auto& ret = state_->return_values[i];
    columns.push_back(Column(columns.size(), ret.name().ToStdString(),
                             sql_argument::TypeToSqlValueType(ret.type())));
  }
  for (size_t i = 0; i < state_->prototype.arguments.size(); ++i) {
    const auto& arg = state_->prototype.arguments[i];

    // Add the "in_" prefix to every argument param to avoid clashes between the
    // output and input parameters.
    columns.push_back(Column(columns.size(), "in_" + arg.name().ToStdString(),
                             sql_argument::TypeToSqlValueType(arg.type()),
                             true));
  }

  std::vector<size_t> primary_keys;

  // Add the "primary key" column. SQLite requires that we provide a column
  // which is non-null and unique. Unfortunately, we have no restrictions on
  // the subqueries so we cannot rely on this constraint being held there.
  // Therefore, we create a "primary key" column which exists purely for SQLite
  // primary key purposes and is equal to the row number.
  columns.push_back(
      Column(columns.size(), "_primary_key", SqlValue::kLong, true));
  primary_keys.emplace_back(columns.size() - 1);

  return SqliteTable::Schema(std::move(columns), std::move(primary_keys));
}

std::unique_ptr<SqliteTable::BaseCursor> RuntimeTableFunction::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this, state_));
}

int RuntimeTableFunction::BestIndex(const QueryConstraints& qc,
                                    BestIndexInfo* info) {
  // Only accept constraint sets where every input parameter has a value.
  size_t seen_argument_constraints = 0;
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];
    seen_argument_constraints +=
        state_->IsArgumentColumn(static_cast<size_t>(cs.column));
  }
  if (seen_argument_constraints < state_->prototype.arguments.size())
    return SQLITE_CONSTRAINT;

  for (size_t i = 0; i < info->sqlite_omit_constraint.size(); ++i) {
    size_t col = static_cast<size_t>(qc.constraints()[i].column);
    if (state_->IsArgumentColumn(col)) {
      info->sqlite_omit_constraint[i] = true;
    }
  }
  return SQLITE_OK;
}

RuntimeTableFunction::Cursor::Cursor(RuntimeTableFunction* table, State* state)
    : SqliteTable::BaseCursor(table), table_(table), state_(state) {
  if (state->reusable_stmt) {
    stmt_ = std::move(state->reusable_stmt);
    state->reusable_stmt = std::nullopt;
    return_stmt_to_state_ = true;
  }
}

RuntimeTableFunction::Cursor::~Cursor() {
  if (return_stmt_to_state_) {
    ResetStatement(stmt_->sqlite_stmt());
    state_->reusable_stmt = std::move(stmt_);
  }
}

base::Status RuntimeTableFunction::Cursor::Filter(const QueryConstraints& qc,
                                                  sqlite3_value** argv,
                                                  FilterHistory) {
  PERFETTO_TP_TRACE(metatrace::Category::FUNCTION_CALL, "TABLE_FUNCTION_CALL",
                    [this](metatrace::Record* r) {
                      r->AddArg("Function",
                                state_->prototype.function_name.c_str());
                    });

  auto col_to_arg_idx = [this](int col) {
    return static_cast<uint32_t>(col) -
           static_cast<uint32_t>(state_->return_values.size());
  };

  size_t seen_argument_constraints = 0;
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];

    // Only consider argument columns (i.e. input parameters) as we're
    // delegating the rest to SQLite.
    if (!state_->IsArgumentColumn(static_cast<size_t>(cs.column)))
      continue;

    // We only support equality constraints as we're expecting "input arguments"
    // to our "function".
    if (!sqlite_utils::IsOpEq(cs.op)) {
      return base::ErrStatus("%s: non-equality constraint passed",
                             state_->prototype.function_name.c_str());
    }

    const auto& arg = state_->prototype.arguments[col_to_arg_idx(cs.column)];
    base::Status status = sqlite_utils::TypeCheckSqliteValue(
        argv[i], sql_argument::TypeToSqlValueType(arg.type()),
        sql_argument::TypeToHumanFriendlyString(arg.type()));
    if (!status.ok()) {
      return base::ErrStatus("%s: argument %s (index %zu) %s",
                             state_->prototype.function_name.c_str(),
                             arg.name().c_str(), i, status.c_message());
    }

    seen_argument_constraints++;
  }

  // Verify that we saw one valid constraint for every input argument.
  if (seen_argument_constraints < state_->prototype.arguments.size()) {
    return base::ErrStatus(
        "%s: missing value for input argument. Saw %zu arguments but expected "
        "%zu",
        state_->prototype.function_name.c_str(), seen_argument_constraints,
        state_->prototype.arguments.size());
  }

  // Prepare the SQL definition as a statement using SQLite.
  // TODO(lalitm): measure and implement whether it would be a good idea to
  // forward constraints here when we build the nested query.
  if (stmt_) {
    // Filter can be called multiple times for the same cursor, so if we
    // already have a statement, reset and reuse it. Otherwise, create a
    // new one.
    ResetStatement(stmt_->sqlite_stmt());
  } else {
    auto stmt = table_->engine_->sqlite_engine()->PrepareStatement(
        state_->sql_defn_str);
    RETURN_IF_ERROR(stmt.status());
    stmt_ = std::move(stmt);
  }

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];

    // Don't deal with any constraints on the output parameters for simplicty.
    // TODO(lalitm): reconsider this decision to allow more efficient queries:
    // we would need to wrap the query in a SELECT * FROM (...) WHERE constraint
    // like we do for SPAN JOIN.
    if (!state_->IsArgumentColumn(static_cast<size_t>(cs.column)))
      continue;

    uint32_t index = col_to_arg_idx(cs.column);
    PERFETTO_DCHECK(index < state_->prototype.arguments.size());

    const auto& arg = state_->prototype.arguments[index];
    auto status = MaybeBindArgument(
        stmt_->sqlite_stmt(), state_->prototype.function_name, arg, argv[i]);
    RETURN_IF_ERROR(status);
  }

  // Reset the next call count - this is necessary because the same cursor
  // can be used for multiple filter operations.
  next_call_count_ = 0;
  return Next();
}

base::Status RuntimeTableFunction::Cursor::Next() {
  is_eof_ = !stmt_->Step();
  next_call_count_++;
  return stmt_->status();
}

bool RuntimeTableFunction::Cursor::Eof() {
  return is_eof_;
}

base::Status RuntimeTableFunction::Cursor::Column(sqlite3_context* ctx, int i) {
  size_t idx = static_cast<size_t>(i);
  if (state_->IsReturnValueColumn(idx)) {
    sqlite3_result_value(ctx, sqlite3_column_value(stmt_->sqlite_stmt(), i));
  } else if (state_->IsArgumentColumn(idx)) {
    // TODO(lalitm): it may be more appropriate to keep a note of the arguments
    // which we passed in and return them here. Not doing this to because it
    // doesn't seem necessary for any useful thing but something which may need
    // to be changed in the future.
    sqlite3_result_null(ctx);
  } else {
    PERFETTO_DCHECK(state_->IsPrimaryKeyColumn(idx));
    sqlite3_result_int(ctx, next_call_count_);
  }
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
