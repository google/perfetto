/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/pipeline/pipeline_syntax.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/tokenizer/sqlite_tokenizer.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {
namespace {

using Token = SqliteTokenizer::Token;

bool IsBareWord(std::string_view str) {
  if (str.empty() || std::isdigit(static_cast<unsigned char>(str[0]))) {
    return false;
  }
  for (char c : str) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
      return false;
    }
  }
  return true;
}

// The name a token spells, with SQL identifier quoting removed, or nothing if
// the token is not a name.
std::optional<std::string> NameOf(const Token& token) {
  std::string_view str = token.str;
  if (IsBareWord(str)) {
    return std::string(str);
  }
  if (token.token_type != sql_token::kId || str.size() < 2) {
    return std::nullopt;
  }
  char open = str.front();
  char close = open == '[' ? ']' : open;
  if ((open != '"' && open != '`' && open != '[') || str.back() != close) {
    return std::nullopt;
  }
  std::string name;
  for (size_t i = 1; i + 1 < str.size(); ++i) {
    name.push_back(str[i]);
    // A quote inside a quoted name is written twice.
    if (open != '[' && str[i] == close) {
      ++i;
    }
  }
  return name;
}

class Parser {
 public:
  explicit Parser(SqlSource sql) : tokenizer_(std::move(sql)) {}

  base::StatusOr<PipelineSyntax> Parse();

 private:
  struct Word {
    Token token;
    // No whitespace or comment separates this word from the one before.
    bool glued;
  };
  // A run of words, [begin, end).
  struct Run {
    size_t begin;
    size_t end;
  };

  void Tokenize();
  bool IsPipeAt(size_t i) const;
  base::StatusOr<Stage> ParseStage(Run);
  base::StatusOr<TreeAccumulate> ParseTreeAccumulate(Run, size_t* pos);

  bool IsKeyword(size_t i, Run run, const char* keyword) const {
    return i < run.end &&
           base::CaseInsensitiveEqual(words_[i].token.str, keyword);
  }
  // Where an error at word `i` of `run` points, which is past the run's last
  // word if `i` is beyond it.
  const Token& At(size_t i, Run run) const {
    if (i < run.end) {
      return words_[i].token;
    }
    return run.end < words_.size() ? words_[run.end].token : end_;
  }
  SqlSource Text(Run run) const {
    return tokenizer_.Substr(words_[run.begin].token, words_[run.end - 1].token,
                             SqliteTokenizer::EndToken::kInclusive);
  }
  base::Status Error(const Token& token, const std::string& message) const {
    return base::ErrStatus("%s%s", tokenizer_.AsTraceback(token).c_str(),
                           message.c_str());
  }

  SqliteTokenizer tokenizer_;
  std::vector<Word> words_;
  Token end_;
};

void Parser::Tokenize() {
  bool glued = true;
  for (;;) {
    Token token = tokenizer_.Next();
    if (token.str.empty()) {
      end_ = token;
      return;
    }
    if (token.token_type == sql_token::kSpace ||
        token.token_type == sql_token::kComment) {
      glued = false;
      continue;
    }
    if (token.token_type == sql_token::kSemi) {
      end_ = token;
      return;
    }
    words_.push_back({token, glued});
    glued = true;
  }
}

// SQLite has no `|>` token, so a pipe is a `|` directly followed by a `>`.
// No valid SQL expression contains those two in a row.
bool Parser::IsPipeAt(size_t i) const {
  return i + 1 < words_.size() && words_[i].token.str == "|" &&
         words_[i + 1].token.str == ">" && words_[i + 1].glued;
}

base::StatusOr<PipelineSyntax> Parser::Parse() {
  Tokenize();
  if (words_.empty() || words_[0].token.token_type != sql_token::kFrom) {
    return Error(words_.empty() ? end_ : words_[0].token,
                 "A pipeline must start with FROM");
  }

  // Split into the FROM clause and one run per stage at every pipe outside
  // parentheses.
  std::vector<Run> runs;
  size_t begin = 1;
  int depth = 0;
  for (size_t i = 1; i < words_.size(); ++i) {
    int type = words_[i].token.token_type;
    if (type == sql_token::kLp) {
      ++depth;
    } else if (type == sql_token::kRp) {
      --depth;
    } else if (depth == 0 && IsPipeAt(i)) {
      runs.push_back({begin, i});
      begin = i + 2;
      ++i;
    }
  }
  runs.push_back({begin, words_.size()});

  if (runs[0].begin == runs[0].end) {
    return Error(At(runs[0].begin, runs[0]),
                 "FROM: expected a table, view or subquery");
  }
  PipelineSyntax syntax{Text(runs[0]), std::nullopt, {}};
  if (runs[0].end - runs[0].begin == 1) {
    syntax.from_name = NameOf(words_[runs[0].begin].token);
  }
  for (size_t i = 1; i < runs.size(); ++i) {
    ASSIGN_OR_RETURN(Stage stage, ParseStage(runs[i]));
    syntax.stages.push_back(std::move(stage));
  }
  return syntax;
}

base::StatusOr<Stage> Parser::ParseStage(Run run) {
  if (run.begin == run.end) {
    return Error(At(run.begin, run), "Expected a pipe operator after |>");
  }
  size_t pos = run.begin;
  if (IsKeyword(pos, run, "TREE") && IsKeyword(pos + 1, run, "ACCUMULATE")) {
    pos += 2;
    ASSIGN_OR_RETURN(TreeAccumulate op, ParseTreeAccumulate(run, &pos));
    if (pos != run.end) {
      return Error(At(pos, run),
                   "TREE ACCUMULATE: expected ',' or the end of the stage");
    }
    return Stage{std::move(op), Text(run)};
  }
  std::string name(words_[pos].token.str);
  if (IsKeyword(pos, run, "TREE") && pos + 1 < run.end) {
    name += " " + std::string(words_[pos + 1].token.str);
  }
  return Error(At(pos, run), "Unsupported pipe operator '" + name +
                                 "': only TREE ACCUMULATE is supported");
}

base::StatusOr<TreeAccumulate> Parser::ParseTreeAccumulate(Run run,
                                                           size_t* pos) {
  TreeAccumulate op;
  size_t i = *pos;
  if (IsKeyword(i, run, "UP")) {
    op.direction = TreeAccumulate::Direction::kUp;
  } else if (IsKeyword(i, run, "DOWN")) {
    op.direction = TreeAccumulate::Direction::kDown;
  } else {
    return Error(At(i, run), "TREE ACCUMULATE: expected UP or DOWN");
  }
  ++i;
  for (;;) {
    size_t start = i;
    if (i >= run.end || !IsBareWord(words_[i].token.str)) {
      return Error(At(i, run),
                   "TREE ACCUMULATE: expected an aggregate, e.g. SUM(col)");
    }
    std::string function = base::ToUpper(std::string(words_[i].token.str));
    ++i;
    if (i >= run.end || words_[i].token.token_type != sql_token::kLp) {
      return Error(At(i, run), "TREE ACCUMULATE: expected '('");
    }
    ++i;
    std::optional<std::string> column;
    if (i < run.end && words_[i].token.token_type == sql_token::kStar) {
      ++i;
    } else if (column = i < run.end ? NameOf(words_[i].token) : std::nullopt;
               column) {
      ++i;
    } else {
      return Error(At(i, run),
                   "TREE ACCUMULATE: the argument of an aggregate must be a "
                   "column name");
    }
    if (i >= run.end || words_[i].token.token_type != sql_token::kRp) {
      return Error(At(i, run), "TREE ACCUMULATE: expected ')'");
    }
    ++i;
    if (!IsKeyword(i, run, "AS")) {
      return Error(At(i, run),
                   "TREE ACCUMULATE: expected AS and a name for the result");
    }
    ++i;
    std::optional<std::string> name =
        i < run.end ? NameOf(words_[i].token) : std::nullopt;
    if (!name) {
      return Error(At(i, run), "TREE ACCUMULATE: expected a name after AS");
    }
    ++i;
    op.aggregates.push_back({std::move(function), std::move(column),
                             std::move(*name), Text({start, i})});
    if (i < run.end && words_[i].token.token_type == sql_token::kComma) {
      ++i;
      continue;
    }
    break;
  }
  *pos = i;
  return op;
}

}  // namespace

base::StatusOr<PipelineSyntax> ParsePipeline(SqlSource sql) {
  return Parser(std::move(sql)).Parse();
}

}  // namespace perfetto::trace_processor::pipeline
