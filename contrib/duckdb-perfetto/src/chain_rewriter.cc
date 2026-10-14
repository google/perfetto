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

#include "contrib/duckdb-perfetto/src/chain_rewriter.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace perfetto::duckdb_ext {
namespace {

constexpr size_t kNone = static_cast<size_t>(-1);

// Strips surrounding whitespace, but keeps a newline ending a line comment.
std::string Trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isspace(static_cast<unsigned char>(s[b])))
    ++b;
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  std::string res = s.substr(b, e - b);
  size_t line = res.rfind('\n');
  if (res.find("--", line == std::string::npos ? 0 : line) != std::string::npos)
    res += '\n';
  return res;
}

// Rewrites the pipelines in one query. Works on the outer tokens, recursing
// into parentheses, so that pipelines can nest.
class ChainRewriter {
 public:
  ChainRewriter(const std::string& s, std::vector<OuterToken> toks)
      : s_(s), toks_(std::move(toks)), close_(toks_.size(), kNone) {
    std::vector<size_t> open;
    for (size_t i = 0; i < toks_.size(); ++i) {
      if (Op(i, '(')) {
        open.push_back(i);
      } else if (Op(i, ')') && !open.empty()) {
        close_[open.back()] = i;
        open.pop_back();
      }
    }
  }

  bool HasPipes() const {
    for (size_t i = 0; i < toks_.size(); ++i) {
      if (Pipe(i))
        return true;
    }
    return false;
  }

  // Rewrites the whole query into |out|.
  bool Rewrite(std::string* out) {
    return Text(0, toks_.size(), 0, s_.size(), true, out);
  }

  const std::string& error() const { return error_; }
  size_t error_offset() const { return error_offset_; }

 private:
  size_t Start(size_t i) const { return toks_[i].start; }
  bool Kw(size_t i, const char* kw) const {
    return i < toks_.size() && IsKeyword(s_, toks_[i], kw);
  }
  bool Op(size_t i, char c) const {
    return i < toks_.size() && IsOperator(s_, toks_[i], c);
  }
  bool Word(size_t i) const {
    return i < toks_.size() && toks_[i].kind == OuterToken::Kind::kWord;
  }
  bool Pipe(size_t i) const {
    return Op(i, '|') && s_.compare(Start(i), 2, "|>") == 0;
  }
  bool Group(size_t i) const { return Op(i, '(') && close_[i] != kNone; }

  bool Fail(size_t offset, std::string msg) {
    error_ = std::move(msg);
    error_offset_ = offset;
    return false;
  }

  // The first token in (i, e), at the nesting level of |i|, which is `|>`,
  // ';' or (if |stop_at_comma|) ','. |e| if there is none.
  size_t Next(size_t i, size_t e, bool stop_at_comma = false) const {
    for (size_t j = i + 1; j < e; ++j) {
      if (Group(j))
        j = close_[j];
      else if (Pipe(j) || Op(j, ';') || (stop_at_comma && Op(j, ',')))
        return j;
    }
    return e;
  }

  // Copies s_[begin, end), which holds tokens [b, e), to |out|, rewriting any
  // pipeline in it. |query_start| says whether a query may start at |b|.
  bool Text(size_t b,
            size_t e,
            size_t begin,
            size_t end,
            bool query_start,
            std::string* out) {
    size_t pos = begin;
    for (size_t i = b; i < e;) {
      if (query_start && Kw(i, "from")) {
        size_t pipe = Next(i, e);
        if (pipe < e && Pipe(pipe)) {
          size_t stop = pipe;
          while (stop < e && Pipe(stop))
            stop = Next(stop, e);
          out->append(s_, pos, Start(i) - pos);
          if (!Pipeline(i, pipe, stop, stop < e ? Start(stop) : end, out))
            return false;
          pos = stop < e ? Start(stop) : end;
          i = stop;
          query_start = false;
          continue;
        }
      }
      if (Pipe(i)) {
        return Fail(Start(i),
                    "|> must follow a pipeline source at the start of a "
                    "query: FROM <table>, FROM (<subquery>) or a PERFETTO "
                    "block");
      }
      if (Group(i)) {
        size_t c = close_[i];
        out->append(s_, pos, Start(i) + 1 - pos);
        if (!Text(i + 1, c, Start(i) + 1, Start(c), true, out))
          return false;
        pos = Start(c);
        i = c;
        continue;
      }
      query_start = Op(i, '(') || Op(i, ';') || Kw(i, "as") ||
                    Kw(i, "explain") || Kw(i, "analyze");
      ++i;
    }
    out->append(s_, pos, end - pos);
    return true;
  }

  // Like Text(), for a part of a pipeline, trimmed.
  bool Part(size_t b, size_t e, size_t begin, size_t end, std::string* out) {
    std::string text;
    if (!Text(b, e, begin, end, false, &text))
      return false;
    *out = Trim(text);
    return true;
  }

  // Rewrites the pipeline starting at the FROM token |from|. Its first pipe
  // is |pipe|, and it ends at token |stop| / offset |end|.
  bool Pipeline(size_t from,
                size_t pipe,
                size_t stop,
                size_t end,
                std::string* out) {
    // The source: a parenthesized subquery, or a possibly qualified name,
    // possibly called (table functions, which is what blocks become).
    size_t j = from + 1;
    if (Group(j)) {
      j = close_[j] + 1;
    } else if (Word(j)) {
      for (++j; Op(j, '.') && Word(j + 1);)
        j += 2;
      if (Group(j))
        j = close_[j] + 1;
    }
    size_t source_end = j;
    std::string alias;
    if (Kw(j, "as") && Word(j + 1) && j + 2 == pipe) {
      alias = Trim(s_.substr(Start(j + 1), Start(pipe) - Start(j + 1)));
      j += 2;
    }
    if (j == from + 1 || j != pipe) {
      return Fail(Start(from + 1),
                  "a pipeline must start from a table, a parenthesized "
                  "subquery or a PERFETTO block, optionally followed by AS "
                  "<alias>");
    }
    std::string source;
    if (!Part(from + 1, source_end, Start(from + 1), Start(source_end),
              &source)) {
      return false;
    }

    // Each stage wraps the query so far in a subquery.
    std::string query;
    auto input = [&] {
      return (query.empty() ? source : "(" + query + ")") +
             (alias.empty() ? "" : " AS " + alias);
    };
    for (size_t p = pipe; p < stop;) {
      size_t next = Next(p, stop);
      size_t stage_end = next < stop ? Start(next) : end;
      size_t k = p + 1;  // The stage keyword.
      if (k == next)
        return Fail(Start(p), "expected a pipe stage after |>");

      // The stage's body: the tokens [b, next).
      std::string b;
      auto body = [&](size_t first, const char* stage) {
        if (first == next)
          return Fail(Start(k), std::string("empty ") + stage + " stage");
        return Part(first, next, Start(first), stage_end, &b);
      };
      if (Kw(k, "where")) {
        if (!body(k + 1, "WHERE"))
          return false;
        query = "SELECT * FROM " + input() + " WHERE " + b;
      } else if (Kw(k, "select")) {
        if (!body(k + 1, "SELECT"))
          return false;
        query = "SELECT " + b + " FROM " + input();
      } else if (Kw(k, "extend")) {
        if (!body(k + 1, "EXTEND"))
          return false;
        query = "SELECT *, " + b + " FROM " + input();
      } else if (Kw(k, "drop")) {
        if (!body(k + 1, "DROP"))
          return false;
        query = "SELECT * EXCLUDE (" + b + ") FROM " + input();
      } else if (Kw(k, "rename")) {
        if (!body(k + 1, "RENAME"))
          return false;
        query = "SELECT * RENAME (" + b + ") FROM " + input();
      } else if (Kw(k, "set")) {
        if (!Set(k, next, stage_end, &b))
          return false;
        query = "SELECT * REPLACE (" + b + ") FROM " + input();
      } else if (Kw(k, "aggregate")) {
        if (!Aggregate(k, next, stage_end, input(), &query))
          return false;
      } else if (Kw(k, "join") || Kw(k, "left") || Kw(k, "right") ||
                 Kw(k, "full") || Kw(k, "inner") || Kw(k, "cross")) {
        if (!body(k, "JOIN"))
          return false;
        query = "SELECT * FROM " + input() + " " + b;
      } else if (Kw(k, "order") && Kw(k + 1, "by")) {
        if (!body(k + 2, "ORDER BY"))
          return false;
        query = "SELECT * FROM " + input() + " ORDER BY " + b;
      } else if (Kw(k, "limit")) {
        if (!body(k + 1, "LIMIT"))
          return false;
        query = "SELECT * FROM " + input() + " LIMIT " + b;
      } else if (Kw(k, "as")) {
        if (!Word(k + 1) || k + 2 != next)
          return Fail(Start(k), "expected a single name after AS");
        alias = Trim(s_.substr(Start(k + 1), stage_end - Start(k + 1)));
      } else if (Kw(k, "interval") || Kw(k, "tree")) {
        return Fail(Start(k),
                    "INTERVAL and TREE stages are only available inside "
                    "PERFETTO blocks");
      } else {
        return Fail(Start(k),
                    "unknown pipe stage: expected WHERE, SELECT, EXTEND, "
                    "SET, DROP, RENAME, AGGREGATE, JOIN, ORDER BY, LIMIT or "
                    "AS");
      }
      p = next;
    }
    out->append(query.empty() ? "SELECT * FROM " + input() : query);
    return true;
  }

  // `SET a = x, b = y` -> `x AS a, y AS b`.
  bool Set(size_t k, size_t next, size_t stage_end, std::string* out) {
    out->clear();
    for (size_t i = k + 1; i < next;) {
      size_t comma = Next(i - 1, next, /*stop_at_comma=*/true);
      size_t item_end = comma < next ? Start(comma) : stage_end;
      if (!Word(i) || !Op(i + 1, '=') || i + 2 >= comma)
        return Fail(Start(i), "expected <column> = <expression>");
      std::string value;
      if (!Part(i + 2, comma, Start(i + 1) + 1, item_end, &value))
        return false;
      *out += (out->empty() ? "" : ", ") + value + " AS " +
              Trim(s_.substr(Start(i), Start(i + 1) - Start(i)));
      i = comma + 1;
    }
    if (out->empty())
      return Fail(Start(k), "expected <column> = <expression> after SET");
    return true;
  }

  // `AGGREGATE <aggregates> [GROUP BY <keys>]` -> the keys, then the
  // aggregates, grouped by the keys' positions.
  bool Aggregate(size_t k,
                 size_t next,
                 size_t stage_end,
                 const std::string& input,
                 std::string* query) {
    size_t group = next;
    size_t keys = 0;
    for (size_t j = k + 1; j < next; ++j) {
      if (Group(j)) {
        j = close_[j];
      } else if (group == next && Kw(j, "group") && Kw(j + 1, "by")) {
        group = j;
        keys = 1;
      } else if (group != next && Op(j, ',')) {
        ++keys;
      }
    }
    if (group == k + 1 || k + 1 == next)
      return Fail(Start(k), "expected aggregates after AGGREGATE");
    std::string aggs;
    size_t aggs_end = group < next ? Start(group) : stage_end;
    if (!Part(k + 1, group, Start(k + 1), aggs_end, &aggs))
      return false;
    if (group == next) {
      *query = "SELECT " + aggs + " FROM " + input;
      return true;
    }
    if (group + 2 >= next)
      return Fail(Start(group), "expected columns after GROUP BY");
    std::string by;
    if (!Part(group + 2, next, Start(group + 2), stage_end, &by))
      return false;
    std::string positions;
    for (size_t i = 1; i <= keys; ++i)
      positions += (i > 1 ? ", " : "") + std::to_string(i);
    *query = "SELECT " + by + ", " + aggs + " FROM " + input + " GROUP BY " +
             positions;
    return true;
  }

  const std::string& s_;
  std::vector<OuterToken> toks_;
  std::vector<size_t> close_;  // For each '(', its matching ')'.
  std::string error_;
  size_t error_offset_ = 0;
};

}  // namespace

RewriteResult RewriteChains(const std::string& query,
                            const OuterTokenizer& tokenize) {
  RewriteResult res;
  if (query.find("|>") == std::string::npos)
    return res;
  ChainRewriter rewriter(query, tokenize(query));
  if (!rewriter.HasPipes())
    return res;
  std::string out;
  if (!rewriter.Rewrite(&out)) {
    res.status = RewriteResult::kError;
    res.error = rewriter.error();
    res.error_offset = rewriter.error_offset();
    return res;
  }
  res.status = RewriteResult::kRewritten;
  res.query = std::move(out);
  return res;
}

RewriteResult RewriteQuery(BlockRewriter* blocks,
                           const std::string& query,
                           const OuterTokenizer& tokenize) {
  RewriteResult res = blocks->Rewrite(query, tokenize);
  if (res.status == RewriteResult::kError)
    return res;
  bool rewritten = res.status == RewriteResult::kRewritten;
  RewriteResult chains = RewriteChains(rewritten ? res.query : query, tokenize);
  if (chains.status == RewriteResult::kNoBlocks)
    return res;
  if (chains.status == RewriteResult::kError) {
    // Map the offset back from the block-rewritten query to |query|.
    size_t off = chains.error_offset;
    for (const auto& r : res.replacements) {
      if (off < r.new_begin)
        break;
      if (off < r.new_end) {
        chains.error_offset = r.begin;  // Point at the block itself.
        break;
      }
      chains.error_offset = off - r.new_end + r.end;
    }
  }
  return chains;
}

}  // namespace perfetto::duckdb_ext
