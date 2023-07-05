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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_PERFETTO_SQL_PARSER_H_
#define SRC_TRACE_PROCESSOR_SQLITE_PERFETTO_SQL_PARSER_H_

#include <string_view>
#include <variant>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/sqlite_tokenizer.h"

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
  struct SqliteSql {
    std::string_view sql;
    uint32_t global_pos;

    bool operator==(const SqliteSql& o) const {
      return sql == o.sql && global_pos == o.global_pos;
    }
  };
  using Statement = std::variant<SqliteSql>;

  // Creates a new SQL parser with the a block of PerfettoSQL statements.
  // Concretely, the passed string can contain >1 statement.
  explicit PerfettoSqlParser(const char* sql);

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

  // Returns the error status for the parser. This will be |base::OkStatus()|
  // until
  const base::Status& status() const { return status_; }

 private:
  SqliteTokenizer tokenizer_;
  const char* start_ = nullptr;
  base::Status status_;
  std::optional<Statement> statement_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_PERFETTO_SQL_PARSER_H_
