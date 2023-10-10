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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_PREPROCESSOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_PREPROCESSOR_H_

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_tokenizer.h"

namespace perfetto {
namespace trace_processor {

// Preprocessor for PerfettoSQL statements. The main responsiblity of this
// class is to perform similar functions to the C/C++ preprocessor (e.g.
// expanding macros). It is also responsible for splitting the given SQL into
// statements.
class PerfettoSqlPreprocessor {
 public:
  struct Macro {
    bool replace;
    std::string name;
    std::vector<std::string> args;
    SqlSource sql;
  };

  // Creates a preprocessor acting on the given SqlSource.
  explicit PerfettoSqlPreprocessor(
      SqlSource,
      const base::FlatHashMap<std::string, Macro>&);

  // Preprocesses the next SQL statement. Returns true if a statement was
  // successfully preprocessed and false if EOF was reached or the statement was
  // not preprocessed correctly.
  //
  // Note: if this function returns false, callers *must* call |status()|: it
  // is undefined behaviour to not do so.
  bool NextStatement();

  // Returns the error status for the parser. This will be |base::OkStatus()|
  // until an unrecoverable error is encountered.
  const base::Status& status() const { return status_; }

  // Returns the most-recent preprocessed SQL statement.
  //
  // Note: this function must not be called unless |NextStatement()| returned
  // true.
  SqlSource& statement() { return *statement_; }

 private:
  struct MacroInvocation {
    const Macro* macro;
    std::unordered_map<std::string, SqlSource> arg_bindings;
  };
  struct InvocationArg {
    std::optional<SqlSource> arg;
    bool has_more;
  };

  base::Status ErrorAtToken(const SqliteTokenizer& tokenizer,
                            const SqliteTokenizer::Token& token,
                            const char* error);
  base::StatusOr<SqlSource> RewriteInternal(
      const SqlSource&,
      const std::unordered_map<std::string, SqlSource>& arg_bindings);

  base::StatusOr<MacroInvocation> ParseMacroInvocation(
      SqliteTokenizer& tokenizer,
      SqliteTokenizer::Token& token,
      const SqliteTokenizer::Token& name_token,
      const std::unordered_map<std::string, SqlSource>& arg_bindings);
  base::StatusOr<InvocationArg> ParseMacroInvocationArg(
      SqliteTokenizer& tokenizer,
      SqliteTokenizer::Token& token,
      bool has_prev_args);

  SqliteTokenizer global_tokenizer_;
  const base::FlatHashMap<std::string, Macro>* macros_ = nullptr;
  std::unordered_set<std::string> seen_macros_;
  std::optional<SqlSource> statement_;
  base::Status status_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_PREPROCESSOR_H_
