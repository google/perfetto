/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_PARSER_STATE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_PARSER_STATE_H_

#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_grammar_interface.h"
#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/perfetto_sql/tokenizer/sqlite_tokenizer.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor {

// Helper to convert PerfettoSqlToken to SqliteTokenizer::Token
inline SqliteTokenizer::Token PerfettoSqlTokenToToken(
    const PerfettoSqlToken& token) {
  return SqliteTokenizer::Token{std::string_view(token.ptr, token.n), 0};
}

struct PerfettoSqlParserState {
  explicit PerfettoSqlParserState(
      SqlSource source,
      const base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro>&
          macros)
      : tokenizer{SqlSource::FromTraceProcessorImplementation("")},
        preprocessor(std::move(source), macros) {}

  void ErrorAtToken(const char* msg, const PerfettoSqlToken& token) {
    status = base::ErrStatus(
        "%s%s", tokenizer.AsTraceback(PerfettoSqlTokenToToken(token)).c_str(),
        msg);
  }

  // Current statement being built
  std::optional<PerfettoSqlParser::Statement> current_statement;

  // Tokenizer for the current statement
  SqliteTokenizer tokenizer;

  // Preprocessor for handling SQL statements
  PerfettoSqlPreprocessor preprocessor;

  // Error handling
  base::Status status;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_PARSER_STATE_H_
