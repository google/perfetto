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

#include "contrib/duckdb-perfetto/src/block_rewriter.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::duckdb_ext {
namespace {

using Kind = OuterToken::Kind;

// What the outer query expects at a given point, as far as blocks care.
enum class Context {
  kQuery,     // Statement start, or after '(', ';' or AS.
  kTableRef,  // After FROM, JOIN or ','.
  kOther,     // Anywhere else, where `perfetto(x)` may be a function call.
};

// Whether |t| is the unquoted keyword |kw|, ignoring case.
bool IsKeyword(const std::string& s, const OuterToken& t, std::string_view kw) {
  if (t.kind != Kind::kWord ||
      !base::CaseInsensitiveEqual(
          std::string_view(s).substr(t.start, kw.size()), kw)) {
    return false;
  }
  size_t end = t.start + kw.size();
  return end == s.size() ||
         !(isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_');
}

bool IsOperator(const std::string& s, const OuterToken& t, char c) {
  return t.kind == Kind::kOperator && s[t.start] == c;
}

Context ContextAfter(const std::string& s, const OuterToken& t) {
  if (IsKeyword(s, t, "from") || IsKeyword(s, t, "join") ||
      IsOperator(s, t, ',')) {
    return Context::kTableRef;
  }
  if (IsKeyword(s, t, "as") || IsOperator(s, t, '(') || IsOperator(s, t, ';'))
    return Context::kQuery;
  return Context::kOther;
}

// The significant tokens of |s| from |begin| onwards, as read by syntaqlite's
// PerfettoSQL tokenizer.
class PerfettoSqlTokens {
 public:
  PerfettoSqlTokens(const std::string& s, size_t begin)
      : s_(s), tok_(syntaqlite_tokenizer_create_perfetto(nullptr)) {
    syntaqlite_tokenizer_reset(tok_, s.data() + begin,
                               static_cast<SyntaqliteLength>(s.size() - begin));
  }
  ~PerfettoSqlTokens() { syntaqlite_tokenizer_destroy(tok_); }

  PerfettoSqlTokens(const PerfettoSqlTokens&) = delete;
  PerfettoSqlTokens& operator=(const PerfettoSqlTokens&) = delete;

  // Skips whitespace and comments. Returns false at the end of the input.
  bool Next(SyntaqliteToken* t) {
    while (syntaqlite_tokenizer_next(tok_, t)) {
      if (t->type != SYNTAQLITE_TK_SPACE && t->type != SYNTAQLITE_TK_COMMENT)
        return true;
    }
    return false;
  }

  // Offset of |t| in the full string.
  size_t Offset(const SyntaqliteToken& t) const {
    return static_cast<size_t>(t.text - s_.data());
  }

 private:
  const std::string& s_;
  SyntaqliteTokenizer* tok_;
};

// The name an identifier token refers to, with "double quotes" removed.
std::string IdentifierName(const SyntaqliteToken& t) {
  std::string text(t.text, t.length);
  if (text.size() >= 2 && text.front() == '"')
    return base::ReplaceAll(text.substr(1, text.size() - 2), "\"\"", "\"");
  return text;
}

struct Block {
  size_t start;  // Offset of the PERFETTO keyword.
  std::string alias;
  size_t body_begin;
};

// Reads a block opener, `PERFETTO ( alias ) (` or (if |allow_unaliased|)
// `PERFETTO (`, starting at the PERFETTO keyword at |start|.
std::optional<Block> ReadOpener(const std::string& s,
                                size_t start,
                                bool allow_unaliased) {
  PerfettoSqlTokens toks(s, start);
  SyntaqliteToken t[5];
  size_t n = 0;
  while (n < 5 && toks.Next(&t[n]))
    ++n;
  if (n < 2 || t[1].type != SYNTAQLITE_TK_LP)
    return std::nullopt;
  if (n == 5 && t[2].type == SYNTAQLITE_TK_ID &&
      t[3].type == SYNTAQLITE_TK_RP && t[4].type == SYNTAQLITE_TK_LP) {
    return Block{start, IdentifierName(t[2]), toks.Offset(t[4]) + 1};
  }
  if (allow_unaliased)
    return Block{start, "", toks.Offset(t[1]) + 1};
  return std::nullopt;
}

// Finds the parenthesis closing a block body which starts at |begin|.
std::optional<size_t> FindBodyEnd(const std::string& s, size_t begin) {
  PerfettoSqlTokens toks(s, begin);
  int depth = 1;
  SyntaqliteToken t;
  while (toks.Next(&t)) {
    if (t.type == SYNTAQLITE_TK_LP) {
      ++depth;
    } else if (t.type == SYNTAQLITE_TK_RP && --depth == 0) {
      return toks.Offset(t);
    }
  }
  return std::nullopt;
}

struct SyntaxError {
  std::string message;
  size_t offset;  // Relative to the body.
};

// Parses |body| with a syntaqlite PerfettoSQL parser.
std::optional<SyntaxError> ValidateBody(SyntaqliteParser* p,
                                        const std::string& body) {
  syntaqlite_parser_reset(p, body.data(),
                          static_cast<SyntaqliteLength>(body.size()));
  std::optional<SyntaxError> err;
  for (;;) {
    int32_t r = syntaqlite_parser_next(p);
    if (r == SYNTAQLITE_PARSE_DONE)
      break;
    if (r == SYNTAQLITE_PARSE_ERROR) {
      // The error offset is statement-relative; shift it by the statement's
      // position in the body.
      SyntaqliteDocOffset stmt_offset = 0;
      syntaqlite_parser_text(p, &stmt_offset, nullptr);
      SyntaqliteStmtOffset off = syntaqlite_result_error_offset(p);
      const char* msg = syntaqlite_result_error_msg(p);
      err = SyntaxError{msg ? msg : "syntax error",
                        stmt_offset + (off == 0xFFFFFFFF ? 0 : off)};
      break;
    }
  }
  return err;
}

std::string QuoteLiteral(const std::string& s) {
  return "'" + base::ReplaceAll(s, "'", "''") + "'";
}

// Finds the first block at or after |pos|, using the outer tokenizer to skip
// everything which is DuckDB syntax. Updates |*ctx| as it goes.
std::optional<Block> FindBlock(const std::string& s,
                               size_t pos,
                               const OuterTokenizer& tokenize,
                               Context* ctx) {
  std::string rest = s.substr(pos);
  for (const OuterToken& t : tokenize(rest)) {
    if (IsKeyword(rest, t, "perfetto")) {
      if (auto block = ReadOpener(s, pos + t.start, *ctx != Context::kOther))
        return block;
    }
    *ctx = ContextAfter(rest, t);
  }
  return std::nullopt;
}

}  // namespace

BlockRewriter::BlockRewriter() = default;

BlockRewriter::~BlockRewriter() {
  for (SyntaqliteParser* p : parsers_)
    syntaqlite_parser_destroy(p);
}

SyntaqliteParser* BlockRewriter::AcquireParser() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!parsers_.empty()) {
      SyntaqliteParser* p = parsers_.back();
      parsers_.pop_back();
      return p;
    }
  }
  SyntaqliteParser* p = syntaqlite_parser_create_perfetto(nullptr);
  // Macro calls are kept verbatim: their definitions live in TraceProcessor.
  syntaqlite_parser_set_macro_fallback(p, 1);
  return p;
}

void BlockRewriter::ReleaseParser(SyntaqliteParser* p) {
  std::lock_guard<std::mutex> lock(mu_);
  parsers_.push_back(p);
}

RewriteResult BlockRewriter::Rewrite(const std::string& s,
                                     const OuterTokenizer& tokenize) {
  RewriteResult res;
  std::string out;
  Context ctx = Context::kQuery;
  size_t pos = 0;
  while (pos < s.size()) {
    std::optional<Block> block = FindBlock(s, pos, tokenize, &ctx);
    if (!block)
      break;
    std::optional<size_t> body_end = FindBodyEnd(s, block->body_begin);
    if (!body_end) {
      res.status = RewriteResult::kError;
      res.error = "PERFETTO block is missing its closing parenthesis";
      res.error_offset = block->start;
      return res;
    }
    std::string body =
        s.substr(block->body_begin, *body_end - block->body_begin);
    SyntaqliteParser* parser = AcquireParser();
    std::optional<SyntaxError> err = ValidateBody(parser, body);
    ReleaseParser(parser);
    if (err) {
      res.status = RewriteResult::kError;
      res.error = "PerfettoSQL syntax error in PERFETTO block: " + err->message;
      res.error_offset = block->body_begin + err->offset;
      return res;
    }
    out.append(s, pos, block->start - pos);
    if (ctx != Context::kTableRef)
      out += "FROM ";
    out += "perfetto_query(" + QuoteLiteral(block->alias) + ", " +
           QuoteLiteral(body) + ")";
    res.status = RewriteResult::kRewritten;
    // Resume tokenizing the outer query from just after the block.
    pos = *body_end + 1;
    ctx = Context::kOther;
  }
  if (res.status == RewriteResult::kRewritten) {
    out.append(s, pos, std::string::npos);
    res.query = std::move(out);
  }
  return res;
}

}  // namespace perfetto::duckdb_ext
