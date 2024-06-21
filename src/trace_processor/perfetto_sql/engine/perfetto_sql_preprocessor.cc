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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_preprocessor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_tokenizer.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor {
namespace {

base::Status ErrorAtToken(const SqliteTokenizer& tokenizer,
                          const SqliteTokenizer::Token& token,
                          const char* error) {
  std::string traceback = tokenizer.AsTraceback(token);
  return base::ErrStatus("%s%s", traceback.c_str(), error);
}

struct InvocationArg {
  std::optional<SqlSource> arg;
  bool has_more;
};

base::StatusOr<InvocationArg> ParseMacroInvocationArg(
    SqliteTokenizer& tokenizer,
    SqliteTokenizer::Token& tok,
    bool has_prev_args) {
  uint32_t nested_parens = 0;
  bool seen_token_in_arg = false;
  auto start = tokenizer.NextNonWhitespace();
  for (tok = start;; tok = tokenizer.NextNonWhitespace()) {
    if (tok.IsTerminal()) {
      if (tok.token_type == SqliteTokenType::TK_SEMI) {
        // TODO(b/290185551): add a link to macro documentation.
        return ErrorAtToken(tokenizer, tok,
                            "Semi-colon is not allowed in macro invocation");
      }
      // TODO(b/290185551): add a link to macro documentation.
      return ErrorAtToken(tokenizer, tok, "Macro invocation not complete");
    }

    bool is_arg_terminator = tok.token_type == SqliteTokenType::TK_RP ||
                             tok.token_type == SqliteTokenType::TK_COMMA;
    if (nested_parens == 0 && is_arg_terminator) {
      bool token_required =
          has_prev_args || tok.token_type != SqliteTokenType::TK_RP;
      if (!seen_token_in_arg && token_required) {
        // TODO(b/290185551): add a link to macro documentation.
        return ErrorAtToken(tokenizer, tok, "Macro arg is empty");
      }
      return InvocationArg{
          seen_token_in_arg ? std::make_optional(tokenizer.Substr(start, tok))
                            : std::optional<SqlSource>(std::nullopt),
          tok.token_type == SqliteTokenType::TK_COMMA,
      };
    }
    seen_token_in_arg = true;

    if (tok.token_type == SqliteTokenType::TK_LP) {
      nested_parens++;
      continue;
    }
    if (tok.token_type == SqliteTokenType::TK_RP) {
      nested_parens--;
      continue;
    }
  }
}

base::StatusOr<std::optional<SqlSource>> ExecuteStringify(
    const SqliteTokenizer& tokenizer,
    const SqliteTokenizer::Token& name_token,
    const std::vector<SqlSource>& args) {
  if (args.size() != 1) {
    return ErrorAtToken(tokenizer, name_token,
                        "stringify: stringify must have exactly one argument");
  }
  SqliteTokenizer t(args[0]);
  auto tok = t.NextNonWhitespace();
  if (tok.token_type == SqliteTokenType::TK_VARIABLE) {
    return {std::nullopt};
  }
  std::string res = "'" + args[0].sql() + "'";
  return {SqlSource::FromTraceProcessorImplementation(std::move(res))};
}

}  // namespace

PerfettoSqlPreprocessor::PerfettoSqlPreprocessor(
    SqlSource source,
    const base::FlatHashMap<std::string, Macro>& macros)
    : global_tokenizer_(std::move(source)), macros_(&macros) {}

bool PerfettoSqlPreprocessor::NextStatement() {
  PERFETTO_CHECK(status_.ok());

  // Skip through any number of semi-colons (representing empty statements).
  SqliteTokenizer::Token tok = global_tokenizer_.NextNonWhitespace();
  while (tok.token_type == SqliteTokenType::TK_SEMI) {
    tok = global_tokenizer_.NextNonWhitespace();
  }

  // If we still see a terminal token at this point, we must have hit EOF.
  if (tok.IsTerminal()) {
    PERFETTO_DCHECK(tok.token_type != SqliteTokenType::TK_SEMI);
    return false;
  }

  SqlSource stmt =
      global_tokenizer_.Substr(tok, global_tokenizer_.NextTerminal());
  auto stmt_or = RewriteInternal(stmt, {});
  if (stmt_or.ok()) {
    statement_ = std::move(*stmt_or);
    return true;
  }
  status_ = stmt_or.status();
  return false;
}

base::StatusOr<SqlSource> PerfettoSqlPreprocessor::RewriteInternal(
    const SqlSource& source,
    const std::unordered_map<std::string, SqlSource>& arg_bindings) {
  SqlSource::Rewriter rewriter(source);
  SqliteTokenizer tokenizer(source);
  for (SqliteTokenizer::Token tok = tokenizer.NextNonWhitespace(), prev;;
       prev = tok, tok = tokenizer.NextNonWhitespace()) {
    if (tok.IsTerminal()) {
      break;
    }
    if (tok.token_type == SqliteTokenType::TK_VARIABLE &&
        !seen_macros_.empty()) {
      PERFETTO_CHECK(tok.str.size() >= 2);
      if (tok.str[0] != '$') {
        return ErrorAtToken(tokenizer, tok, "Variables must start with $");
      }
      auto binding_it = arg_bindings.find(std::string(tok.str.substr(1)));
      if (binding_it == arg_bindings.end()) {
        return ErrorAtToken(tokenizer, tok, "Variable not found");
      }
      tokenizer.RewriteToken(rewriter, tok, binding_it->second);
      continue;
    }
    if (tok.token_type != SqliteTokenType::TK_ILLEGAL || tok.str != "!") {
      continue;
    }

    const auto& name_token = prev;
    if (name_token.token_type == SqliteTokenType::TK_VARIABLE) {
      // TODO(b/290185551): add a link to macro documentation.
      return ErrorAtToken(tokenizer, name_token,
                          "Macro name cannot be a variable");
    }
    if (name_token.token_type != SqliteTokenType::TK_ID) {
      // TODO(b/290185551): add a link to macro documentation.
      return ErrorAtToken(tokenizer, name_token, "Macro invocation is invalid");
    }

    // Go to the opening parenthesis of the macro invocation.
    tok = tokenizer.NextNonWhitespace();

    std::string macro_name(name_token.str);
    ASSIGN_OR_RETURN(std::vector<SqlSource> token_list,
                     ParseTokenList(tokenizer, tok, arg_bindings));
    if (macro_name == "__intrinsic_stringify") {
      ASSIGN_OR_RETURN(std::optional<SqlSource> res,
                       ExecuteStringify(tokenizer, name_token, token_list));
      if (res) {
        tokenizer.Rewrite(rewriter, prev, tok, *std::move(res),
                          SqliteTokenizer::EndToken::kInclusive);
      }
      continue;
    }
    if (macro_name == "__intrinsic_token_zip_join") {
      ASSIGN_OR_RETURN(
          std::optional<SqlSource> res,
          ExecuteTokenZipJoin(tokenizer, name_token, std::move(token_list)));
      if (res) {
        tokenizer.Rewrite(rewriter, prev, tok, *std::move(res),
                          SqliteTokenizer::EndToken::kInclusive);
      }
      continue;
    }
    if (macro_name == "__intrinsic_token_comma") {
      if (!token_list.empty()) {
        return ErrorAtToken(tokenizer, name_token,
                            "token_comma: no arguments allowd");
      }
      tokenizer.Rewrite(rewriter, prev, tok,
                        SqlSource::FromTraceProcessorImplementation(","),
                        SqliteTokenizer::EndToken::kInclusive);
      continue;
    }
    ASSIGN_OR_RETURN(SqlSource invocation,
                     ExecuteMacroInvocation(tokenizer, prev, macro_name,
                                            std::move(token_list)));
    tokenizer.Rewrite(rewriter, prev, tok, std::move(invocation),
                      SqliteTokenizer::EndToken::kInclusive);
  }
  return std::move(rewriter).Build();
}

base::StatusOr<std::vector<SqlSource>> PerfettoSqlPreprocessor::ParseTokenList(
    SqliteTokenizer& tokenizer,
    SqliteTokenizer::Token& tok,
    const std::unordered_map<std::string, SqlSource>& bindings) {
  if (tok.token_type != SqliteTokenType::TK_LP) {
    return ErrorAtToken(tokenizer, tok, "( expected to open token list");
  }
  std::vector<SqlSource> tokens;
  bool has_more = true;
  while (has_more) {
    ASSIGN_OR_RETURN(InvocationArg invocation_arg,
                     ParseMacroInvocationArg(tokenizer, tok, !tokens.empty()));
    if (invocation_arg.arg) {
      ASSIGN_OR_RETURN(SqlSource res,
                       RewriteInternal(invocation_arg.arg.value(), bindings));
      tokens.emplace_back(std::move(res));
    }
    has_more = invocation_arg.has_more;
  }
  return tokens;
}

base::StatusOr<SqlSource> PerfettoSqlPreprocessor::ExecuteMacroInvocation(
    const SqliteTokenizer& tokenizer,
    const SqliteTokenizer::Token& name_token,
    const std::string& macro_name,
    std::vector<SqlSource> token_list) {
  Macro* macro = macros_->Find(macro_name);
  if (!macro) {
    // TODO(b/290185551): add a link to macro documentation.
    base::StackString<1024> err("Macro %s does not exist", macro_name.c_str());
    return ErrorAtToken(tokenizer, name_token, err.c_str());
  }
  if (seen_macros_.count(macro_name)) {
    // TODO(b/290185551): add a link to macro documentation.
    return ErrorAtToken(tokenizer, name_token,
                        "Macros cannot be recursive or mutually recursive");
  }
  if (token_list.size() < macro->args.size()) {
    // TODO(lalitm): add a link to macro documentation.
    return ErrorAtToken(tokenizer, name_token,
                        "Macro invoked with too few args");
  }
  std::unordered_map<std::string, SqlSource> inner_bindings;
  for (auto& t : token_list) {
    inner_bindings.emplace(macro->args[inner_bindings.size()], std::move(t));
  }
  PERFETTO_CHECK(inner_bindings.size() == macro->args.size());

  seen_macros_.emplace(macro->name);
  ASSIGN_OR_RETURN(SqlSource res, RewriteInternal(macro->sql, inner_bindings));
  seen_macros_.erase(macro->name);
  return res;
}

base::StatusOr<std::optional<SqlSource>>
PerfettoSqlPreprocessor::ExecuteTokenZipJoin(
    const SqliteTokenizer& tokenizer,
    const SqliteTokenizer::Token& name_token,
    std::vector<SqlSource> token_list) {
  if (token_list.size() != 4) {
    return ErrorAtToken(tokenizer, name_token,
                        "token_zip_join: must have exactly four args");
  }

  SqliteTokenizer first_tokenizer(std::move(token_list[0]));
  SqliteTokenizer::Token inner_tok = first_tokenizer.NextNonWhitespace();
  if (inner_tok.token_type == SqliteTokenType::TK_VARIABLE) {
    return {std::nullopt};
  }
  ASSIGN_OR_RETURN(std::vector<SqlSource> first_sources,
                   ParseTokenList(first_tokenizer, inner_tok, {}));

  SqliteTokenizer second_tokenizer(std::move(token_list[1]));
  inner_tok = second_tokenizer.NextNonWhitespace();
  if (inner_tok.token_type == SqliteTokenType::TK_VARIABLE) {
    return {std::nullopt};
  }
  ASSIGN_OR_RETURN(std::vector<SqlSource> second_sources,
                   ParseTokenList(second_tokenizer, inner_tok, {}));

  SqliteTokenizer name_tokenizer(token_list[2]);
  inner_tok = name_tokenizer.NextNonWhitespace();
  if (inner_tok.token_type == SqliteTokenType::TK_VARIABLE) {
    return {std::nullopt};
  }

  size_t zip_count = std::min(first_sources.size(), second_sources.size());
  std::vector<std::string> res;
  for (uint32_t i = 0; i < zip_count; ++i) {
    ASSIGN_OR_RETURN(
        SqlSource invocation_res,
        ExecuteMacroInvocation(tokenizer, name_token, token_list[2].sql(),
                               {first_sources[i], second_sources[i]}));
    res.push_back(invocation_res.sql());
  }
  return {SqlSource::FromTraceProcessorImplementation(
      base::Join(res, " " + token_list[3].sql() + " "))};
}

}  // namespace perfetto::trace_processor
