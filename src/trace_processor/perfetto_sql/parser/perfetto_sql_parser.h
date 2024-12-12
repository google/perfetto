/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_PERFETTO_SQL_PARSER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_PERFETTO_SQL_PARSER_H_

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/perfetto_sql/tokenizer/sqlite_tokenizer.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto {
namespace trace_processor {

// Parser for PerfettoSQL statements. This class provides an iterator-style
// interface for reading all PerfettoSQL statements from a block of SQL.
//
// Usage:
// PerfettoSqlParser parser(my_sql_string.c_str());
// while (parser.Next()) {
//   auto& stmt = parser.statement();
//   // Handle |stmt| here
// }
// RETURN_IF_ERROR(r.status());
class PerfettoSqlParser {
 public:
  // Indicates that the specified SQLite SQL was extracted directly from a
  // PerfettoSQL statement and should be directly executed with SQLite.
  struct SqliteSql {};
  // Indicates that the specified SQL was a CREATE PERFETTO FUNCTION statement
  // with the following parameters.
  struct CreateFunction {
    bool replace;
    FunctionPrototype prototype;
    std::string returns;
    SqlSource sql;
    bool is_table;
  };
  // Indicates that the specified SQL was a CREATE PERFETTO TABLE statement
  // with the following parameters.
  struct CreateTable {
    bool replace;
    std::string name;
    // SQL source for the select statement.
    SqlSource sql;
    std::vector<sql_argument::ArgumentDefinition> schema;
  };
  // Indicates that the specified SQL was a CREATE PERFETTO VIEW statement
  // with the following parameters.
  struct CreateView {
    bool replace;
    std::string name;
    // SQL source for the select statement.
    SqlSource select_sql;
    // SQL source corresponding to the rewritten statement creating the
    // underlying view.
    SqlSource create_view_sql;
    std::vector<sql_argument::ArgumentDefinition> schema;
  };
  // Indicates that the specified SQL was a CREATE PERFETTO INDEX statement
  // with the following parameters.
  struct CreateIndex {
    bool replace = false;
    std::string name;
    std::string table_name;
    std::vector<std::string> col_names;
  };
  // Indicates that the specified SQL was a DROP PERFETTO INDEX statement
  // with the following parameters.
  struct DropIndex {
    std::string name;
    std::string table_name;
  };
  // Indicates that the specified SQL was a INCLUDE PERFETTO MODULE statement
  // with the following parameter.
  struct Include {
    std::string key;
  };
  // Indicates that the specified SQL was a CREATE PERFETTO MACRO statement
  // with the following parameter.
  struct CreateMacro {
    bool replace;
    SqlSource name;
    std::vector<std::pair<SqlSource, SqlSource>> args;
    SqlSource returns;
    SqlSource sql;
  };
  using Statement = std::variant<CreateFunction,
                                 CreateIndex,
                                 CreateMacro,
                                 CreateTable,
                                 CreateView,
                                 DropIndex,
                                 Include,
                                 SqliteSql>;

  // Creates a new SQL parser with the a block of PerfettoSQL statements.
  // Concretely, the passed string can contain >1 statement.
  explicit PerfettoSqlParser(
      SqlSource,
      const base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro>&);

  // Attempts to parse to the next statement in the SQL. Returns true if
  // a statement was successfully parsed and false if EOF was reached or the
  // statement was not parsed correctly.
  //
  // Note: if this function returns false, callers *must* call |status()|: it
  // is undefined behaviour to not do so.
  bool Next();

  // Returns the current statement which was parsed. This function *must not* be
  // called unless |Next()| returned true.
  Statement& statement() {
    PERFETTO_DCHECK(statement_.has_value());
    return statement_.value();
  }

  // Returns the full statement which was parsed. This should return
  // |statement()| and Perfetto SQL code that's in front. This function *must
  // not* be called unless |Next()| returned true.
  const SqlSource& statement_sql() const {
    PERFETTO_CHECK(statement_sql_);
    return *statement_sql_;
  }

  // Returns the error status for the parser. This will be |base::OkStatus()|
  // until an unrecoverable error is encountered.
  const base::Status& status() const { return status_; }

 private:
  // This cannot be moved because we keep pointers into |sql_| in
  // |preprocessor_|.
  PerfettoSqlParser(PerfettoSqlParser&&) = delete;
  PerfettoSqlParser& operator=(PerfettoSqlParser&&) = delete;

  // Most of the code needs sql_argument::ArgumentDefinition, but we explcitly
  // track raw arguments separately, as macro implementations need access to
  // the underlying tokens.
  struct RawArgument {
    SqliteTokenizer::Token name;
    SqliteTokenizer::Token type;
  };

  bool ParseCreatePerfettoFunction(
      bool replace,
      SqliteTokenizer::Token first_non_space_token);

  enum class TableOrView {
    kTable,
    kView,
  };
  bool ParseCreatePerfettoTableOrView(
      bool replace,
      SqliteTokenizer::Token first_non_space_token,
      TableOrView table_or_view);

  bool ParseIncludePerfettoModule(SqliteTokenizer::Token first_non_space_token);

  bool ParseCreatePerfettoMacro(bool replace);

  bool ParseCreatePerfettoIndex(bool replace,
                                SqliteTokenizer::Token first_non_space_token);

  bool ParseDropPerfettoIndex(SqliteTokenizer::Token first_non_space_token);

  // Convert a "raw" argument (i.e. one that points to specific tokens) to the
  // argument definition consumed by the rest of the SQL code.
  // Guarantees to call ErrorAtToken if std::nullopt is returned.
  std::optional<sql_argument::ArgumentDefinition> ResolveRawArgument(
      RawArgument arg);
  // Parse the arguments in their raw token form.
  bool ParseRawArguments(std::vector<RawArgument>&);
  // Same as above, but also convert the raw tokens into argument definitions.
  bool ParseArguments(std::vector<sql_argument::ArgumentDefinition>&);

  bool ErrorAtToken(const SqliteTokenizer::Token&, const char* error, ...);

  PerfettoSqlPreprocessor preprocessor_;
  SqliteTokenizer tokenizer_;

  base::Status status_;
  std::optional<SqlSource> statement_sql_;
  std::optional<Statement> statement_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_PERFETTO_SQL_PARSER_H_
