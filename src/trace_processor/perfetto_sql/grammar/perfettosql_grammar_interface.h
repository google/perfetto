/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_GRAMMAR_INTERFACE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_GRAMMAR_INTERFACE_H_

#include <stddef.h>
#include <stdio.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/perfetto_sql/grammar/perfettosql_grammar.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

#undef NDEBUG

namespace perfetto::trace_processor {

// Basic token structure containing source information.
struct PerfettoSqlToken {
  const char* ptr;  // Pointer to start of token in source
  size_t n;         // Length of token
};

// Overall structure to hold the parsing state.
struct PerfettoSqlParserState;

// List structure for arguments
struct PerfettoSqlArgumentList {
  std::vector<sql_argument::ArgumentDefinition> inner;
};

// List structure for indexed columns
struct PerfettoSqlIndexedColumnList {
  std::vector<std::string> cols;
};

// List structure for macro arguments
struct PerfettoSqlMacroArgumentList {
  std::vector<std::pair<SqlSource, SqlSource>> args;
};

// Return type for functions.
struct PerfettoSqlFnReturnType {
  bool is_table;
  sql_argument::Type scalar_type;
  std::vector<sql_argument::ArgumentDefinition> table_columns;
};

// Parser allocation/deallocation functions
void* PerfettoSqlParseAlloc(void* (*allocator)(size_t),
                            PerfettoSqlParserState*);
void PerfettoSqlParse(void* parser, int token_type, PerfettoSqlToken token);
void PerfettoSqlParseFree(void* parser, void (*free_fn)(void*));

// Error handling
void OnPerfettoSqlSyntaxError(PerfettoSqlParserState*, PerfettoSqlToken*);

// Helper to extract SQL source from a token using the parser state's tokenizer
SqlSource OnPerfettoSqlExtractSource(PerfettoSqlParserState* state,
                                     const PerfettoSqlToken& token);

// Helper to parse SQL argument type from a token
std::optional<sql_argument::Type> OnPerfettoSqlParseType(
    const PerfettoSqlToken& token);

// Helper to report error at a token position
void OnPerfettoSqlError(PerfettoSqlParserState* state,
                        const char* message,
                        const PerfettoSqlToken& token);

// Helper to extract substring between two tokens using tokenizer
SqlSource OnPerfettoSqlSubstr(PerfettoSqlParserState* state,
                              const PerfettoSqlToken& start,
                              const PerfettoSqlToken& end);

// Helper to extract substring with default end token behavior
SqlSource OnPerfettoSqlSubstrDefault(PerfettoSqlParserState* state,
                                     const PerfettoSqlToken& start,
                                     const PerfettoSqlToken& end);

// Helper to get the preprocessor statement
SqlSource OnPerfettoSqlGetPreprocessorStatement(PerfettoSqlParserState* state);

// Helper to rewrite for CREATE VIEW
SqlSource OnPerfettoSqlRewriteView(PerfettoSqlParserState* state,
                                   const PerfettoSqlToken& create_token,
                                   const PerfettoSqlToken& name,
                                   const PerfettoSqlToken& body_start);

// Helper to rewrite for CREATE INDEX
SqlSource OnPerfettoSqlRewriteIndex(PerfettoSqlParserState* state,
                                    const PerfettoSqlToken& create_token,
                                    const PerfettoSqlToken& name);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_GRAMMAR_PERFETTOSQL_GRAMMAR_INTERFACE_H_
