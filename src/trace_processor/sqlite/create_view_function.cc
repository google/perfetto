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

#include "src/trace_processor/sqlite/create_view_function.h"

#include <numeric>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/sqlite/create_function_internal.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

class CreatedViewFunction : public SqliteTable {
 public:
  class Cursor : public SqliteTable::Cursor {
   public:
    explicit Cursor(CreatedViewFunction* table);
    ~Cursor() override;

    int Filter(const QueryConstraints& qc,
               sqlite3_value**,
               FilterHistory) override;
    int Next() override;
    int Eof() override;
    int Column(sqlite3_context* context, int N) override;

   private:
    sqlite3_stmt* stmt_ = nullptr;
    CreatedViewFunction* table_ = nullptr;
    bool is_eof_ = false;
  };

  CreatedViewFunction(sqlite3*, CreateViewFunction::State* state);
  ~CreatedViewFunction() override;

  base::Status Init(int argc, const char* const* argv, Schema*) override;
  std::unique_ptr<SqliteTable::Cursor> CreateCursor() override;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) override;

  static void Register(sqlite3* db, CreateViewFunction::State* state) {
    SqliteTable::Register<CreatedViewFunction>(
        db, state, "internal_view_function_impl", false, true);
  }

 private:
  Schema CreateSchema();

  sqlite3* db_ = nullptr;

  Prototype prototype_;
  std::vector<Prototype::Argument> return_values_;

  std::string prototype_str_;
  std::string sql_defn_str_;

  CreateViewFunction::State* state_;
};

CreatedViewFunction::CreatedViewFunction(sqlite3* db,
                                         CreateViewFunction::State* state)
    : db_(db), state_(state) {}
CreatedViewFunction::~CreatedViewFunction() = default;

base::Status CreatedViewFunction::Init(int argc,
                                       const char* const* argv,
                                       Schema* schema) {
  // The first three args are SQLite ones which we ignore.
  PERFETTO_CHECK(argc == 6);

  prototype_str_ = argv[3];
  std::string return_prototype_str = argv[4];
  sql_defn_str_ = argv[5];

  // SQLite gives us strings with quotes included (i.e. 'string'). Strip these
  // from the front and back.
  prototype_str_ = prototype_str_.substr(1, prototype_str_.size() - 2);
  return_prototype_str =
      return_prototype_str.substr(1, return_prototype_str.size() - 2);
  sql_defn_str_ = sql_defn_str_.substr(1, sql_defn_str_.size() - 2);

  // Parse all the arguments into a more friendly form.
  base::Status status =
      ParsePrototype(base::StringView(prototype_str_), prototype_);
  if (!status.ok()) {
    return base::ErrStatus("CREATE_VIEW_FUNCTION[prototype=%s]: %s",
                           prototype_str_.c_str(), status.c_message());
  }

  // Parse the return type into a enum format.
  status = ParseArgs(return_prototype_str, return_values_);
  if (!status.ok()) {
    return base::ErrStatus(
        "CREATE_VIEW_FUNCTION[prototype=%s, return=%s]: unknown return type "
        "specified",
        prototype_str_.c_str(), return_prototype_str.c_str());
  }

  // Now we've parsed prototype and return values, create the schema.
  *schema = CreateSchema();

  return base::OkStatus();
}

SqliteTable::Schema CreatedViewFunction::CreateSchema() {
  std::vector<Column> columns;
  for (size_t i = 0; i < return_values_.size(); ++i) {
    const auto& ret = return_values_[i];
    columns.push_back(Column(columns.size(), ret.name, ret.type, false));
  }
  for (size_t i = 0; i < prototype_.arguments.size(); ++i) {
    const auto& arg = prototype_.arguments[i];
    columns.push_back(Column(columns.size(), arg.name, arg.type, true));
  }

  std::vector<size_t> primary_keys(return_values_.size());
  std::iota(primary_keys.begin(), primary_keys.end(), 0);

  return SqliteTable::Schema(std::move(columns), std::move(primary_keys));
}

std::unique_ptr<SqliteTable::Cursor> CreatedViewFunction::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this));
}

int CreatedViewFunction::BestIndex(const QueryConstraints& qc,
                                   BestIndexInfo* info) {
  for (size_t i = 0; i < info->sqlite_omit_constraint.size(); ++i) {
    size_t col = static_cast<size_t>(qc.constraints()[i].column);
    if (schema().columns()[col].hidden()) {
      info->sqlite_omit_constraint[i] = true;
    }
  }
  return SQLITE_OK;
}

CreatedViewFunction::Cursor::Cursor(CreatedViewFunction* table)
    : SqliteTable::Cursor(table), table_(table) {}

CreatedViewFunction::Cursor::~Cursor() = default;

int CreatedViewFunction::Cursor::Filter(const QueryConstraints& qc,
                                        sqlite3_value** argv,
                                        FilterHistory) {
  auto col_to_arg_idx = [this](int col) {
    return static_cast<size_t>(col) - table_->return_values_.size();
  };

  size_t seen_hidden_constraints = 0;
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];

    // Only consider hidden columns (i.e. input parameters) as we're delegating
    // the rest to SQLite.
    if (!table_->schema().columns()[static_cast<size_t>(cs.column)].hidden())
      continue;

    // We only support equality constraints as we're expecting "input arguments"
    // to our "function".
    if (!sqlite_utils::IsOpEq(cs.op)) {
      table_->SetErrorMessage(
          sqlite3_mprintf("%s: non-equality constraint passed",
                          table_->prototype_.function_name.c_str()));
      return SQLITE_ERROR;
    }

    const auto& arg = table_->prototype_.arguments[col_to_arg_idx(cs.column)];
    SqlValue::Type expected_type = arg.type;
    base::Status status = TypeCheckSqliteValue(argv[i], expected_type);
    if (!status.ok()) {
      table_->SetErrorMessage(
          sqlite3_mprintf("%s: argument %s (index %u) %s",
                          table_->prototype_.function_name.c_str(),
                          arg.name.c_str(), i, status.c_message()));
      return SQLITE_ERROR;
    }

    seen_hidden_constraints++;
  }

  // Verify that we saw one valid constriant for every input argument.
  if (seen_hidden_constraints < table_->prototype_.arguments.size()) {
    table_->SetErrorMessage(
        sqlite3_mprintf("%s: missing value for input argument",
                        table_->prototype_.function_name.c_str()));
    return SQLITE_ERROR;
  }

  // Prepare the SQL definition as a statement using SQLite.
  // TODO(lalitm): see if we can reuse this prepared statement rather than
  // creating it very time.
  // TODO(lalitm): measure and implement whether it would be a good idea to
  // forward constraints here when we build the nested query.
  ScopedStmt stmt;
  int ret = sqlite3_prepare_v2(table_->db_, table_->sql_defn_str_.data(),
                               static_cast<int>(table_->sql_defn_str_.size()),
                               &stmt_, nullptr);
  stmt.reset(stmt_);
  if (ret != SQLITE_OK) {
    table_->SetErrorMessage(sqlite3_mprintf(
        "%s: Failed to prepare SQL statement for function. "
        "Check the SQL defintion this function for syntax errors. "
        "(SQLite error: %s).",
        table_->prototype_.function_name.c_str(), sqlite3_errmsg(table_->db_)));
    return SQLITE_ERROR;
  }

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];
    const auto& arg = table_->prototype_.arguments[col_to_arg_idx(cs.column)];
    auto status = MaybeBindArgument(stmt_, table_->prototype_.function_name,
                                    arg, argv[i]);
    if (!status.ok()) {
      table_->SetErrorMessage(sqlite3_mprintf("%s", status.c_message()));
      return SQLITE_ERROR;
    }
  }

  ret = Next();
  if (ret != SQLITE_OK)
    return ret;

  // Keep track of the scoped statements in the stmts vector so we can clean
  // all these up before destroying trace processor.
  table_->state_->erase(table_->prototype_.function_name);
  table_->state_->emplace(table_->prototype_.function_name, std::move(stmt));

  return SQLITE_OK;
}

int CreatedViewFunction::Cursor::Next() {
  int ret = sqlite3_step(stmt_);
  is_eof_ = ret == SQLITE_DONE;
  if (ret != SQLITE_ROW && ret != SQLITE_DONE) {
    table_->SetErrorMessage(sqlite3_mprintf(
        "%s: SQLite error while stepping statement: %s",
        table_->prototype_.function_name.c_str(), sqlite3_errmsg(table_->db_)));
    return ret;
  }
  return SQLITE_OK;
}

int CreatedViewFunction::Cursor::Eof() {
  return is_eof_;
}

int CreatedViewFunction::Cursor::Column(sqlite3_context* ctx, int i) {
  size_t idx = static_cast<size_t>(i);
  if (idx < table_->return_values_.size()) {
    sqlite3_result_value(ctx, sqlite3_column_value(stmt_, i));
  } else {
    sqlite3_result_null(ctx);
  }
  return SQLITE_OK;
}

}  // namespace

base::Status CreateViewFunction::Run(CreateViewFunction::Context* ctx,
                                     size_t argc,
                                     sqlite3_value** argv,
                                     SqlValue&,
                                     Destructors&) {
  if (argc != 3) {
    return base::ErrStatus(
        "CREATE_VIEW_FUNCTION: invalid number of args; expected %u, received "
        "%zu",
        3u, argc);
  }

  sqlite3_value* prototype_value = argv[0];
  sqlite3_value* return_prototype_value = argv[1];
  sqlite3_value* sql_defn_value = argv[2];

  // Type check all the arguments.
  {
    auto type_check = [prototype_value](sqlite3_value* value,
                                        SqlValue::Type type, const char* desc) {
      base::Status status = TypeCheckSqliteValue(value, type);
      if (!status.ok()) {
        return base::ErrStatus("CREATE_VIEW_FUNCTION[prototype=%s]: %s %s",
                               sqlite3_value_text(prototype_value), desc,
                               status.c_message());
      }
      return base::OkStatus();
    };

    RETURN_IF_ERROR(type_check(prototype_value, SqlValue::Type::kString,
                               "function prototype (first argument)"));
    RETURN_IF_ERROR(type_check(return_prototype_value, SqlValue::Type::kString,
                               "return prototype (second argument)"));
    RETURN_IF_ERROR(type_check(sql_defn_value, SqlValue::Type::kString,
                               "SQL definition (third argument)"));
  }

  // Extract the arguments from the value wrappers.
  auto extract_string = [](sqlite3_value* value) -> const char* {
    return reinterpret_cast<const char*>(sqlite3_value_text(value));
  };
  const char* prototype_str = extract_string(prototype_value);
  const char* return_prototype_str = extract_string(return_prototype_value);
  const char* sql_defn_str = extract_string(sql_defn_value);

  base::StringView function_name;
  RETURN_IF_ERROR(ParseFunctionName(prototype_str, function_name));

  base::StackString<1024> sql(
      "CREATE VIRTUAL TABLE IF NOT EXISTS %s USING "
      "INTERNAL_VIEW_FUNCTION_IMPL('%s', '%s', '%s');",
      function_name.ToStdString().c_str(), prototype_str, return_prototype_str,
      sql_defn_str);

  ScopedSqliteString errmsg;
  char* errmsg_raw = nullptr;
  int ret = sqlite3_exec(ctx->db, sql.c_str(), nullptr, nullptr, &errmsg_raw);
  errmsg.reset(errmsg_raw);
  if (ret != SQLITE_OK)
    return base::ErrStatus("%s", errmsg.get());

  // CREATE_VIEW_FUNCTION doesn't have a return value so just don't sent |out|.
  return base::OkStatus();
}

void CreateViewFunction::RegisterTable(sqlite3* db,
                                       CreateViewFunction::State* state) {
  CreatedViewFunction::Register(db, state);
}

}  // namespace trace_processor
}  // namespace perfetto
