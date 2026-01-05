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

#include "src/trace_processor/perfetto_sql/parser/perfetto_sql_parser.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_grammar_interface.h"
#include "src/trace_processor/perfetto_sql/grammar/perfettosql_parser_state.h"
#include "src/trace_processor/perfetto_sql/parser/function_util.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/perfetto_sql/tokenizer/sqlite_tokenizer.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/util/sql_argument.h"

namespace perfetto::trace_processor {

namespace {

using Token = SqliteTokenizer::Token;
using Statement = PerfettoSqlParser::Statement;

PerfettoSqlToken TokenToPerfettoSqlToken(const Token& token) {
  return PerfettoSqlToken{token.str.data(), token.str.size()};
}

}  // namespace

// Grammar interface implementation

SqlSource OnPerfettoSqlExtractSource(PerfettoSqlParserState* state,
                                     const PerfettoSqlToken& token) {
  return state->tokenizer.SubstrToken(PerfettoSqlTokenToToken(token));
}

std::optional<sql_argument::Type> OnPerfettoSqlParseType(
    const PerfettoSqlToken& token) {
  return sql_argument::ParseType(base::StringView(token.ptr, token.n));
}

void OnPerfettoSqlError(PerfettoSqlParserState* state,
                        const char* message,
                        const PerfettoSqlToken& token) {
  state->ErrorAtToken(message, token);
}

SqlSource OnPerfettoSqlSubstr(PerfettoSqlParserState* state,
                              const PerfettoSqlToken& start,
                              const PerfettoSqlToken& end) {
  return state->tokenizer.Substr(PerfettoSqlTokenToToken(start),
                                 PerfettoSqlTokenToToken(end),
                                 SqliteTokenizer::EndToken::kInclusive);
}

SqlSource OnPerfettoSqlSubstrDefault(PerfettoSqlParserState* state,
                                     const PerfettoSqlToken& start,
                                     const PerfettoSqlToken& end) {
  return state->tokenizer.Substr(PerfettoSqlTokenToToken(start),
                                 PerfettoSqlTokenToToken(end));
}

SqlSource OnPerfettoSqlGetPreprocessorStatement(PerfettoSqlParserState* state) {
  return state->preprocessor.statement();
}

SqlSource OnPerfettoSqlRewriteView(PerfettoSqlParserState* state,
                                   const PerfettoSqlToken& create_token,
                                   const PerfettoSqlToken& name,
                                   const PerfettoSqlToken& body_start) {
  SqlSource header = SqlSource::FromTraceProcessorImplementation(
      "CREATE VIEW " + std::string(name.ptr, name.n) + " AS ");
  SqlSource::Rewriter rewriter(state->preprocessor.statement());
  state->tokenizer.Rewrite(rewriter, PerfettoSqlTokenToToken(create_token),
                           PerfettoSqlTokenToToken(body_start), header);
  return std::move(rewriter).Build();
}

SqlSource OnPerfettoSqlRewriteIndex(PerfettoSqlParserState* state,
                                    const PerfettoSqlToken& create_token,
                                    const PerfettoSqlToken& name) {
  SqlSource header = SqlSource::FromTraceProcessorImplementation(
      "CREATE INDEX " + std::string(name.ptr, name.n));
  SqlSource::Rewriter rewriter(state->preprocessor.statement());
  state->tokenizer.Rewrite(rewriter, PerfettoSqlTokenToToken(create_token),
                           PerfettoSqlTokenToToken(create_token), header,
                           SqliteTokenizer::EndToken::kExclusive);
  return std::move(rewriter).Build();
}

void OnPerfettoSqlSyntaxError(PerfettoSqlParserState* state,
                              PerfettoSqlToken* token) {
  if (token->n == 0) {
    state->ErrorAtToken("incomplete input", *token);
  } else {
    state->ErrorAtToken("syntax error", *token);
  }
}

PerfettoSqlParser::PerfettoSqlParser(
    SqlSource source,
    const base::FlatHashMap<std::string, PerfettoSqlPreprocessor::Macro>&
        macros)
    : parser_state_(std::make_unique<PerfettoSqlParserState>(std::move(source),
                                                             macros)) {}

PerfettoSqlParser::~PerfettoSqlParser() = default;

bool PerfettoSqlParser::Next() {
  PERFETTO_DCHECK(parser_state_->status.ok());

  parser_state_->current_statement = std::nullopt;
  statement_sql_ = std::nullopt;

  if (!parser_state_->preprocessor.NextStatement()) {
    parser_state_->status = parser_state_->preprocessor.status();
    return false;
  }
  parser_state_->tokenizer.Reset(parser_state_->preprocessor.statement());

  auto* parser = PerfettoSqlParseAlloc(malloc, parser_state_.get());
  auto guard = base::OnScopeExit([&]() { PerfettoSqlParseFree(parser, free); });

  enum { kEof, kSemicolon, kNone } eof = kNone;
  for (Token token = parser_state_->tokenizer.Next();;
       token = parser_state_->tokenizer.Next()) {
    if (!parser_state_->status.ok()) {
      return false;
    }
    if (token.IsTerminal()) {
      if (eof == kNone) {
        PerfettoSqlParse(parser, TK_SEMI, TokenToPerfettoSqlToken(token));
        eof = kSemicolon;
        continue;
      }
      if (eof == kSemicolon) {
        PerfettoSqlParse(parser, 0, TokenToPerfettoSqlToken(token));
        eof = kEof;
        continue;
      }
      if (!parser_state_->current_statement) {
        parser_state_->current_statement = SqliteSql{};
      }
      statement_sql_ = parser_state_->preprocessor.statement();
      return true;
    }
    if (token.token_type == TK_SPACE || token.token_type == TK_COMMENT) {
      continue;
    }
    PerfettoSqlParse(parser, token.token_type, TokenToPerfettoSqlToken(token));
  }
}

const Statement& PerfettoSqlParser::statement() const {
  PERFETTO_DCHECK(parser_state_->current_statement.has_value());
  return *parser_state_->current_statement;
}

const base::Status& PerfettoSqlParser::status() const {
  return parser_state_->status;
}

}  // namespace perfetto::trace_processor
