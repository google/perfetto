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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_view_function.h"

#include <numeric>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_function_internal.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

class CreatedViewFunction final
    : public TypedSqliteTable<CreatedViewFunction, PerfettoSqlEngine*> {
 public:
  class Cursor final : public SqliteTable::BaseCursor {
   public:
    explicit Cursor(CreatedViewFunction* table);
    ~Cursor() final;

    base::Status Filter(const QueryConstraints& qc,
                        sqlite3_value**,
                        FilterHistory);
    base::Status Next();
    bool Eof();
    base::Status Column(sqlite3_context* context, int N);

   private:
    std::optional<SqliteEngine::PreparedStatement> stmt_;
    CreatedViewFunction* table_ = nullptr;
    bool is_eof_ = false;
    int next_call_count_ = 0;
  };

  CreatedViewFunction(sqlite3*, PerfettoSqlEngine*);
  ~CreatedViewFunction() final;

  base::Status Init(int argc, const char* const* argv, Schema*) final;
  std::unique_ptr<SqliteTable::BaseCursor> CreateCursor() final;
  int BestIndex(const QueryConstraints& qc, BestIndexInfo* info) final;

 private:
  Schema CreateSchema();

  bool IsReturnValueColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i < return_values_.size();
  }

  bool IsArgumentColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i >= return_values_.size() &&
           (i - return_values_.size()) < prototype_.arguments.size();
  }

  bool IsPrimaryKeyColumn(size_t i) const {
    PERFETTO_DCHECK(i < schema().columns().size());
    return i == (return_values_.size() + prototype_.arguments.size());
  }

  PerfettoSqlEngine* engine_ = nullptr;

  Prototype prototype_;
  std::vector<sql_argument::ArgumentDefinition> return_values_;

  std::string prototype_str_;
  std::string sql_defn_str_;
};

CreatedViewFunction::CreatedViewFunction(sqlite3*, PerfettoSqlEngine* engine)
    : engine_(engine) {}
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
  status = sql_argument::ParseArgumentDefinitions(return_prototype_str,
                                                  return_values_);
  if (!status.ok()) {
    return base::ErrStatus(
        "CREATE_VIEW_FUNCTION[prototype=%s, return=%s]: unknown return type "
        "specified",
        prototype_str_.c_str(), return_prototype_str.c_str());
  }

  // Verify that the provided SQL prepares to a statement correctly.
  auto res = engine_->sqlite_engine()->PrepareStatement(
      SqlSource::FromFunction(sql_defn_str_, prototype_str_));
  RETURN_IF_ERROR(res.status());

  // Verify that every argument name in the function appears in the
  // argument list.
  //
  // We intentionally loop from 1 to |used_param_count| because SQL
  // parameters are 1-indexed *not* 0-indexed.
  int used_param_count = sqlite3_bind_parameter_count(res->sqlite_stmt());
  for (int i = 1; i <= used_param_count; ++i) {
    const char* name = sqlite3_bind_parameter_name(res->sqlite_stmt(), i);

    if (!name) {
      return base::ErrStatus(
          "%s: \"Nameless\" SQL parameters cannot be used in the SQL "
          "statements of view functions.",
          prototype_.function_name.c_str());
    }

    if (!base::StringView(name).StartsWith("$")) {
      return base::ErrStatus(
          "%s: invalid parameter name %s used in the SQL definition of "
          "the view function: all parameters must be prefixed with '$' not ':' "
          "or '@'.",
          prototype_.function_name.c_str(), name);
    }

    auto it =
        std::find_if(prototype_.arguments.begin(), prototype_.arguments.end(),
                     [name](const sql_argument::ArgumentDefinition& arg) {
                       return arg.dollar_name() == name;
                     });
    if (it == prototype_.arguments.end()) {
      return base::ErrStatus(
          "%s: parameter %s does not appear in the list of arguments in the "
          "prototype of the view function.",
          prototype_.function_name.c_str(), name);
    }
  }

  // Verify that the prepared statement column count matches the return
  // count.
  uint32_t col_count =
      static_cast<uint32_t>(sqlite3_column_count(res->sqlite_stmt()));
  if (col_count != return_values_.size()) {
    return base::ErrStatus(
        "%s: number of return values %u does not match SQL statement column "
        "count %zu.",
        prototype_.function_name.c_str(), col_count, return_values_.size());
  }

  // Verify that the return names matches the prepared statment column names.
  for (uint32_t i = 0; i < col_count; ++i) {
    const char* name =
        sqlite3_column_name(res->sqlite_stmt(), static_cast<int>(i));
    if (name != return_values_[i].name()) {
      return base::ErrStatus(
          "%s: column %s at index %u does not match return value name %s.",
          prototype_.function_name.c_str(), name, i,
          return_values_[i].name().c_str());
    }
  }

  // Now we've parsed prototype and return values, create the schema.
  *schema = CreateSchema();

  return base::OkStatus();
}

SqliteTable::Schema CreatedViewFunction::CreateSchema() {
  std::vector<Column> columns;
  for (size_t i = 0; i < return_values_.size(); ++i) {
    const auto& ret = return_values_[i];
    columns.push_back(Column(columns.size(), ret.name().ToStdString(),
                             sql_argument::TypeToSqlValueType(ret.type())));
  }
  for (size_t i = 0; i < prototype_.arguments.size(); ++i) {
    const auto& arg = prototype_.arguments[i];

    // Add the "in_" prefix to every argument param to avoid clashes between the
    // output and input parameters.
    columns.push_back(Column(columns.size(), "in_" + arg.name().ToStdString(),
                             sql_argument::TypeToSqlValueType(arg.type()),
                             true));
  }

  std::vector<size_t> primary_keys;

  // Add the "primary key" column. SQLite requires that we provide a column
  // which is non-null and unique. Unfortunately, we have no restrictions on
  // the subqueries so we cannot rely on this constriant being held there.
  // Therefore, we create a "primary key" column which exists purely for SQLite
  // primary key purposes and is equal to the row number.
  columns.push_back(
      Column(columns.size(), "_primary_key", SqlValue::kLong, true));
  primary_keys.emplace_back(columns.size() - 1);

  return SqliteTable::Schema(std::move(columns), std::move(primary_keys));
}

std::unique_ptr<SqliteTable::BaseCursor> CreatedViewFunction::CreateCursor() {
  return std::unique_ptr<Cursor>(new Cursor(this));
}

int CreatedViewFunction::BestIndex(const QueryConstraints& qc,
                                   BestIndexInfo* info) {
  // Only accept constraint sets where every input parameter has a value.
  size_t seen_argument_constraints = 0;
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];
    seen_argument_constraints +=
        IsArgumentColumn(static_cast<size_t>(cs.column));
  }
  if (seen_argument_constraints < prototype_.arguments.size())
    return SQLITE_CONSTRAINT;

  for (size_t i = 0; i < info->sqlite_omit_constraint.size(); ++i) {
    size_t col = static_cast<size_t>(qc.constraints()[i].column);
    if (IsArgumentColumn(col)) {
      info->sqlite_omit_constraint[i] = true;
    }
  }
  return SQLITE_OK;
}

CreatedViewFunction::Cursor::Cursor(CreatedViewFunction* table)
    : SqliteTable::BaseCursor(table), table_(table) {}

CreatedViewFunction::Cursor::~Cursor() = default;

base::Status CreatedViewFunction::Cursor::Filter(const QueryConstraints& qc,
                                                 sqlite3_value** argv,
                                                 FilterHistory) {
  PERFETTO_TP_TRACE(metatrace::Category::FUNCTION, "CREATE_VIEW_FUNCTION",
                    [this](metatrace::Record* r) {
                      r->AddArg("Function",
                                table_->prototype_.function_name.c_str());
                    });

  auto col_to_arg_idx = [this](int col) {
    return static_cast<uint32_t>(col) -
           static_cast<uint32_t>(table_->return_values_.size());
  };

  size_t seen_argument_constraints = 0;
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];

    // Only consider argument columns (i.e. input parameters) as we're
    // delegating the rest to SQLite.
    if (!table_->IsArgumentColumn(static_cast<size_t>(cs.column)))
      continue;

    // We only support equality constraints as we're expecting "input arguments"
    // to our "function".
    if (!sqlite_utils::IsOpEq(cs.op)) {
      return base::ErrStatus("%s: non-equality constraint passed",
                             table_->prototype_.function_name.c_str());
    }

    const auto& arg = table_->prototype_.arguments[col_to_arg_idx(cs.column)];
    base::Status status = sqlite_utils::TypeCheckSqliteValue(
        argv[i], sql_argument::TypeToSqlValueType(arg.type()),
        sql_argument::TypeToHumanFriendlyString(arg.type()));
    if (!status.ok()) {
      return base::ErrStatus("%s: argument %s (index %zu) %s",
                             table_->prototype_.function_name.c_str(),
                             arg.name().c_str(), i, status.c_message());
    }

    seen_argument_constraints++;
  }

  // Verify that we saw one valid constriant for every input argument.
  if (seen_argument_constraints < table_->prototype_.arguments.size()) {
    return base::ErrStatus(
        "%s: missing value for input argument. Saw %zu arguments but expected "
        "%zu",
        table_->prototype_.function_name.c_str(), seen_argument_constraints,
        table_->prototype_.arguments.size());
  }

  // Prepare the SQL definition as a statement using SQLite.
  // TODO(lalitm): see if we can reuse this prepared statement rather than
  // creating it very time.
  // TODO(lalitm): measure and implement whether it would be a good idea to
  // forward constraints here when we build the nested query.
  auto res = table_->engine_->sqlite_engine()->PrepareStatement(
      SqlSource::FromFunction(table_->sql_defn_str_, table_->prototype_str_));
  RETURN_IF_ERROR(res->status());
  stmt_ = std::move(res.value());

  // Bind all the arguments to the appropriate places in the function.
  for (size_t i = 0; i < qc.constraints().size(); ++i) {
    const auto& cs = qc.constraints()[i];

    // Don't deal with any constraints on the output parameters for simplicty.
    // TODO(lalitm): reconsider this decision to allow more efficient queries:
    // we would need to wrap the query in a SELECT * FROM (...) WHERE constraint
    // like we do for SPAN JOIN.
    if (!table_->IsArgumentColumn(static_cast<size_t>(cs.column)))
      continue;

    uint32_t index = col_to_arg_idx(cs.column);
    PERFETTO_DCHECK(index < table_->prototype_.arguments.size());

    const auto& arg = table_->prototype_.arguments[index];
    auto status = MaybeBindArgument(
        stmt_->sqlite_stmt(), table_->prototype_.function_name, arg, argv[i]);
    RETURN_IF_ERROR(status);
  }

  // Reset the next call count - this is necessary because the same cursor
  // can be used for multiple filter operations.
  next_call_count_ = 0;
  return Next();
}

base::Status CreatedViewFunction::Cursor::Next() {
  is_eof_ = !stmt_->Step();
  next_call_count_++;
  return stmt_->status();
}

bool CreatedViewFunction::Cursor::Eof() {
  return is_eof_;
}

base::Status CreatedViewFunction::Cursor::Column(sqlite3_context* ctx, int i) {
  size_t idx = static_cast<size_t>(i);
  if (table_->IsReturnValueColumn(idx)) {
    sqlite3_result_value(ctx, sqlite3_column_value(stmt_->sqlite_stmt(), i));
  } else if (table_->IsArgumentColumn(idx)) {
    // TODO(lalitm): it may be more appropriate to keep a note of the arguments
    // which we passed in and return them here. Not doing this to because it
    // doesn't seem necessary for any useful thing but something which may need
    // to be changed in the future.
    sqlite3_result_null(ctx);
  } else {
    PERFETTO_DCHECK(table_->IsPrimaryKeyColumn(idx));
    sqlite3_result_int(ctx, next_call_count_);
  }
  return base::OkStatus();
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
      base::Status status = sqlite_utils::TypeCheckSqliteValue(value, type);
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

  static constexpr char kSqlTemplate[] = R"""(
    DROP TABLE IF EXISTS %s;

    CREATE VIRTUAL TABLE %s
    USING INTERNAL_VIEW_FUNCTION_IMPL('%s', '%s', '%s');
  )""";
  std::string function_name_str = function_name.ToStdString();

  NullTermStringView sql_defn(sql_defn_str);
  std::string formatted_sql(sql_defn.size() + 1024, '\0');
  base::SprintfTrunc(formatted_sql.data(), formatted_sql.size(), kSqlTemplate,
                     function_name_str.c_str(), function_name_str.c_str(),
                     prototype_str, return_prototype_str, sql_defn_str);

  // CREATE_VIEW_FUNCTION doesn't have a return value so just don't sent |out|.
  auto res = ctx->Execute(
      SqlSource::FromFunction(std::move(formatted_sql), prototype_str));
  return res.status();
}

void RegisterCreateViewFunctionModule(PerfettoSqlEngine* engine) {
  engine->sqlite_engine()->RegisterVirtualTableModule<CreatedViewFunction>(
      "internal_view_function_impl", engine,
      SqliteTable::TableType::kExplicitCreate, false);
}

}  // namespace trace_processor
}  // namespace perfetto
