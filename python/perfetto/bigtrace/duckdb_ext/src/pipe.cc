// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// pf_compile_pipe(sql): compiles `|>` chain syntax, the stages PerfettoSQL's
// pipe syntax (docs/analysis/perfetto-sql-pipe-syntax.md) shares with
// GoogleSQL's, into plain DuckDB SQL. For example
//
//   FROM slices AS s
//   |> WHERE s.dur > 0
//   |> AGGREGATE count(*) AS n GROUP BY s._trace_uuid
//   |> ORDER BY n DESC
//   |> LIMIT 10
//
// becomes
//
//   SELECT * FROM (SELECT s._trace_uuid, count(*) AS n FROM slices AS s
//   WHERE (s.dur > 0) GROUP BY s._trace_uuid) ORDER BY n DESC LIMIT 10
//
// Stages fold into one SELECT for as long as SQL's clause order (FROM/JOIN,
// WHERE, GROUP BY, SELECT, ORDER BY, LIMIT) allows, except that ORDER BY,
// which sorts the rows of the stage before it, only follows FROM/JOIN and
// WHERE. Otherwise the query so far becomes a subquery named after the table
// alias in scope, so that `alias.column` keeps working until a SELECT or
// AGGREGATE (which drop aliases, as in PerfettoSQL) or `|> AS` (which
// replaces them).
//
// INTERVAL, TREE and GRAPH stages and sources (docs/intervals.md, trees.md,
// graphs.md) compile to the pf_* operators of intervals.cc, trees.cc and
// graphs.cc, e.g. `INTERVAL INTERSECTION OF (slices AS s, sched AS c) PER
// _trace_uuid, utid` or `|> TREE ACCUMULATE UP SUM(dur) AS total PER
// _trace_uuid`.
//
// Pipelines may appear wherever a query can: as a statement, a CTE body or a
// parenthesized subquery. Text outside pipelines is copied unchanged, except
// for duration literals (`16ms` -> 16000000). Text without any `|>` is
// returned as is.
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common.h"  // no-include-violation-check

namespace {

using Strs = std::vector<std::string>;

enum class Tk {
  kEnd,
  kPipe,   // |>
  kOpen,   // ( [ {
  kClose,  // ) ] }
  kComma,
  kSemi,
  kEq,
  kStar,
  kDot,
  kWord,    // keyword, identifier, number or quoted identifier
  kString,  // '...'
  kOther,   // any other operator
};

struct Tok {
  Tk kind;
  size_t begin;
  size_t end;
  std::string upper;  // The upper-cased text of unquoted words, else empty.
};

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Splits `s` into tokens, skipping whitespace and comments. Returns false,
// setting `error`, on an unterminated literal or unbalanced brackets.
bool Tokenize(const std::string& s, std::vector<Tok>* out, std::string* error) {
  size_t n = s.size();
  int depth = 0;
  for (size_t i = 0; i < n;) {
    char c = s[i];
    size_t start = i;
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
      continue;
    }
    if (c == '-' && i + 1 < n && s[i + 1] == '-') {
      while (i < n && s[i] != '\n')
        i++;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t close = s.find("*/", i + 2);
      if (close == std::string::npos) {
        *error = "unterminated /* comment";
        return false;
      }
      i = close + 2;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      // Quotes are escaped by doubling them.
      for (i++;; i++) {
        if (i >= n) {
          *error = c == '\'' ? "unterminated string literal"
                             : "unterminated quoted identifier";
          return false;
        }
        if (s[i] == c) {
          if (i + 1 < n && s[i + 1] == c) {
            i++;
            continue;
          }
          break;
        }
      }
      i++;
      out->push_back({c == '\'' ? Tk::kString : Tk::kWord, start, i, ""});
      continue;
    }
    if (IsIdentChar(c)) {
      bool number = std::isdigit(static_cast<unsigned char>(c)) != 0;
      while (i < n && (IsIdentChar(s[i]) ||
                       (number && s[i] == '.' && i + 1 < n &&
                        std::isdigit(static_cast<unsigned char>(s[i + 1]))))) {
        i++;
      }
      std::string upper = s.substr(start, i - start);
      for (char& ch : upper)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      out->push_back({Tk::kWord, start, i, std::move(upper)});
      continue;
    }
    Tk kind = Tk::kOther;
    i++;
    switch (c) {
      case '(':
      case '[':
      case '{':
        kind = Tk::kOpen;
        depth++;
        break;
      case ')':
      case ']':
      case '}':
        kind = Tk::kClose;
        if (--depth < 0) {
          *error = std::string("unbalanced '") + c + "'";
          return false;
        }
        break;
      case ',':
        kind = Tk::kComma;
        break;
      case ';':
        kind = Tk::kSemi;
        break;
      case '=':
        kind = Tk::kEq;
        break;
      case '*':
        kind = Tk::kStar;
        break;
      case '.':
        kind = Tk::kDot;
        break;
      case '|':
        if (i < n && s[i] == '>') {
          kind = Tk::kPipe;
          i++;
        }
        break;
      default:
        break;
    }
    out->push_back({kind, start, i, ""});
  }
  if (depth != 0) {
    *error = "unbalanced '('";
    return false;
  }
  out->push_back({Tk::kEnd, n, n, ""});
  return true;
}

// `16ms` -> "16000000": a number with a unit of ns, us, ms, s, m, h or d is a
// duration in nanoseconds. Returns "" for any other word.
std::string DurationLiteral(const std::string& w) {
  size_t i = 0;
  while (i < w.size() &&
         (std::isdigit(static_cast<unsigned char>(w[i])) || w[i] == '.'))
    i++;
  if (i == 0 || i == w.size() || i > 18 ||
      !std::isdigit(static_cast<unsigned char>(w[0])))
    return "";
  std::string unit = w.substr(i);
  for (char& c : unit)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const std::pair<const char*, int64_t> kUnits[] = {
      {"ns", 1},
      {"us", 1000},
      {"ms", 1000000},
      {"s", 1000000000},
      {"m", 60 * 1000000000LL},
      {"h", 3600 * 1000000000LL},
      {"d", 86400 * 1000000000LL}};
  std::string num = w.substr(0, i);
  for (const auto& [name, mult] : kUnits) {
    if (unit != name)
      continue;
    double value = std::stod(num) * static_cast<double>(mult);
    if (num.find('.') != num.rfind('.') || value >= 9e18)
      return "";
    if (num.find('.') == std::string::npos)
      return std::to_string(std::stoll(num) * mult);
    return std::to_string(std::llround(value));
  }
  return "";
}

std::string Join(const Strs& v, const char* sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++)
    r += (i ? sep : "") + v[i];
  return r;
}

std::string Lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string SqlQuote(const std::string& s) {
  std::string q = "'";
  for (char c : s)
    q += c == '\'' ? std::string("''") : std::string(1, c);
  return q + "'";
}

std::string SqlQuoteList(const Strs& v) {
  Strs q;
  for (const auto& s : v)
    q.push_back(SqlQuote(s));
  return q.empty() ? "[]::VARCHAR[]" : "[" + Join(q, ", ") + "]::VARCHAR[]";
}

// `s` as an identifier: quoted unless it is a plain word or already quoted.
std::string SqlIdent(const std::string& s) {
  if (!s.empty() && (s[0] == '"' || s[0] == '`'))
    return s;
  bool plain = !s.empty();
  for (char c : s)
    plain &= IsIdentChar(c) && c != '$';
  if (plain)
    return s;
  std::string q = "\"";
  for (char c : s)
    q += c == '"' ? std::string("\"\"") : std::string(1, c);
  return q + "\"";
}

// The operators' generated SQL groups by an extra constant key when DuckDB
// runs with several threads (see GroupBy in sql_gen.cc); always the case in
// the Bigtrace server.
constexpr int64_t kThreads = 2;

// SQL clauses in the order a SELECT evaluates them.
enum Clause { kFrom, kWhere, kGroupBy, kSelect, kOrderBy, kLimit };

// The SELECT a pipeline's stages are folded into.
struct Select {
  std::string from;
  Strs where;
  std::string items;  // Empty for `*`.
  std::string group_by;
  std::string order_by;
  std::string limit;
  Clause last = kFrom;  // The last clause set.
  std::string alias;    // The table alias that a subquery keeps, if any.

  std::string Sql() const {
    std::string sql = "SELECT " + (items.empty() ? "*" : items);
    sql += " FROM " + from;
    if (!where.empty())
      sql += " WHERE (" + Join(where, ") AND (") + ")";
    if (!group_by.empty())
      sql += " GROUP BY " + group_by;
    if (!order_by.empty())
      sql += " ORDER BY " + order_by;
    if (!limit.empty())
      sql += " LIMIT " + limit;
    return sql;
  }
};

class PipeCompiler {
 public:
  PipeCompiler(const std::string& src, std::vector<Tok> toks)
      : src_(src), toks_(std::move(toks)) {}

  // Compiles every `;`-separated statement. Returns false, setting `error`,
  // if the input is not valid chain syntax.
  bool CompileScript(std::string* out, std::string* error) {
    bool has_pipe = false;
    for (size_t i = 0; i < toks_.size(); i++)
      has_pipe |= Is(i, Tk::kPipe) || IsOperatorSource(i);
    if (!has_pipe) {
      *out = src_;
      return true;
    }
    size_t end = toks_.size() - 1;  // Excludes kEnd.
    std::vector<size_t> semis =
        TopLevel(0, end, [&](size_t i) { return Is(i, Tk::kSemi); });
    semis.push_back(end);
    Strs stmts;
    size_t begin = 0;
    for (size_t semi : semis) {
      if (semi > begin)
        stmts.push_back(CompileQuery(begin, semi));
      begin = semi + 1;
    }
    *error = error_;
    *out = Join(stmts, ";\n");
    return error_.empty();
  }

 private:
  std::string Text(size_t i) const {
    return src_.substr(toks_[i].begin, toks_[i].end - toks_[i].begin);
  }
  // The source text of tokens [b, e), for error messages.
  std::string Excerpt(size_t b, size_t e) const {
    if (b >= e)
      return "";
    std::string s =
        src_.substr(toks_[b].begin, toks_[e - 1].end - toks_[b].begin);
    return s.size() > 60 ? s.substr(0, 57) + "..." : s;
  }
  bool IsKw(size_t i, const char* kw) const {
    return i < toks_.size() && toks_[i].upper == kw;
  }
  bool Is(size_t i, Tk kind) const {
    return i < toks_.size() && toks_[i].kind == kind;
  }
  void Fail(const std::string& msg) {
    if (error_.empty())
      error_ = msg;
  }

  // The closing bracket of the one opened at `open`.
  size_t Close(size_t open) const {
    int depth = 0;
    for (size_t i = open;; i++) {
      if (Is(i, Tk::kOpen))
        depth++;
      else if (Is(i, Tk::kClose) && --depth == 0)
        return i;
    }
  }
  // Positions of top-level (bracket depth 0) tokens in [b, e) for which
  // `pred` holds.
  template <typename Pred>
  std::vector<size_t> TopLevel(size_t b, size_t e, Pred pred) const {
    std::vector<size_t> r;
    for (size_t i = b; i < e; i++) {
      if (Is(i, Tk::kOpen))
        i = Close(i);
      else if (pred(i))
        r.push_back(i);
    }
    return r;
  }
  std::vector<size_t> TopLevelCommas(size_t b, size_t e) const {
    return TopLevel(b, e, [&](size_t i) { return Is(i, Tk::kComma); });
  }
  bool HasTopLevelPipe(size_t b, size_t e) const {
    return !TopLevel(b, e, [&](size_t i) { return Is(i, Tk::kPipe); }).empty();
  }

  // Copies [b, e), compiling the queries in brackets and rewriting duration
  // literals. In a SELECT list (`select_list`), `* EXCEPT (...)` becomes
  // DuckDB's `* EXCLUDE (...)`.
  std::string Rewrite(size_t b, size_t e, bool select_list = false) {
    std::string out;
    for (size_t i = b; i < e; i++) {
      if (i > b)
        out += src_.substr(toks_[i - 1].end, toks_[i].begin - toks_[i - 1].end);
      if (Is(i, Tk::kOpen)) {
        size_t close = Close(i);
        out += Text(i) + CompileQuery(i + 1, close) + Text(close);
        i = close;
      } else if (select_list && IsKw(i, "EXCEPT") && i > b &&
                 Is(i - 1, Tk::kStar) && Is(i + 1, Tk::kOpen)) {
        out += "EXCLUDE";
      } else if (std::string d = DurationLiteral(toks_[i].upper); !d.empty()) {
        out += d;
      } else if (!struct_aliases_.empty() && Is(i, Tk::kWord) &&
                 Is(i + 1, Tk::kDot) && (i == b || !Is(i - 1, Tk::kDot)) &&
                 struct_aliases_.count(Lower(Text(i)))) {
        out += SqlIdent(struct_aliases_[Lower(Text(i))]);
      } else {
        out += Text(i);
      }
    }
    return out;
  }

  // Compiles a query (or any bracketed text): `WITH ... <query>`, a pipeline,
  // or anything else, which is copied by Rewrite().
  std::string CompileQuery(size_t b, size_t e) {
    Strs ctes;
    std::string with = "WITH ";
    size_t cur = b;
    if (IsKw(b, "WITH")) {
      cur++;
      if (IsKw(cur, "RECURSIVE")) {
        with += "RECURSIVE ";
        cur++;
      }
      // name [(cols)] AS [[NOT] MATERIALIZED] (query), ...
      for (;;) {
        size_t head = cur;
        if (cur >= e || !Is(cur++, Tk::kWord))
          return Rewrite(b, e);
        if (cur < e && Is(cur, Tk::kOpen))
          cur = Close(cur) + 1;
        if (cur >= e || !IsKw(cur++, "AS"))
          return Rewrite(b, e);
        if (cur < e && IsKw(cur, "NOT"))
          cur++;
        if (cur < e && IsKw(cur, "MATERIALIZED"))
          cur++;
        if (cur >= e || !Is(cur, Tk::kOpen))
          return Rewrite(b, e);
        cur = Close(cur) + 1;
        ctes.push_back(Rewrite(head, cur));
        if (!Is(cur, Tk::kComma))
          break;
        cur++;
      }
    }
    if (!HasTopLevelPipe(cur, e) && !IsOperatorSource(cur))
      return Rewrite(b, e);
    std::string body = CompilePipeline(cur, e, &ctes);
    return ctes.empty() ? body : with + Join(ctes, ", ") + " " + body;
  }

  // Compiles `source |> stage |> ...` in [b, e). `FORK AS` adds to `ctes`.
  std::string CompilePipeline(size_t b, size_t e, Strs* ctes) {
    std::vector<size_t> pipes =
        TopLevel(b, e, [&](size_t i) { return Is(i, Tk::kPipe); });
    pipes.push_back(e);
    auto outer_aliases = std::move(struct_aliases_);
    struct_aliases_.clear();
    Select q;
    if (!CompileOperatorSource(b, pipes[0], pipes, &q))
      CompileSource(b, pipes[0], &q);
    for (size_t i = 0; i + 1 < pipes.size() && error_.empty(); i++)
      CompileStage(pipes[i] + 1, pipes[i + 1], &q, ctes);
    struct_aliases_ = std::move(outer_aliases);
    return q.Sql();
  }

  // `FROM <from clause>`, or a query (in brackets or not) used as a table.
  void CompileSource(size_t b, size_t e, Select* q) {
    if (IsKw(b, "FROM") && b + 1 < e) {
      q->from = Rewrite(b + 1, e);
      q->alias = ImplicitAlias(b + 1, e);
    } else if (IsKw(b, "SELECT") || IsKw(b, "WITH") || IsKw(b, "VALUES")) {
      q->from = "(" + CompileQuery(b, e) + ")";
    } else if (Is(b, Tk::kOpen) && Close(b) + 1 == e) {
      q->from = "(" + CompileQuery(b + 1, e - 1) + ")";
    } else {
      Fail("a pipeline starts with FROM <table> or a query, got '" +
           Excerpt(b, e) + "'");
    }
  }

  // The table alias of a FROM clause `rel [[AS] alias]`, where `rel` is a
  // table name, `(query)` or `function(...)`, or "" for anything else.
  std::string ImplicitAlias(size_t b, size_t e) const {
    size_t cur = b;
    std::string name;
    if (Is(cur, Tk::kWord)) {
      name = Text(cur++);
      while (Is(cur, Tk::kDot) && Is(cur + 1, Tk::kWord)) {
        name = Text(cur + 1);
        cur += 2;
      }
    }
    if (Is(cur, Tk::kOpen)) {
      cur = Close(cur) + 1;
      name.clear();
    }
    if (cur == b)
      return "";
    if (cur == e)
      return name;
    if (IsKw(cur, "AS"))
      cur++;
    return cur + 1 == e && Is(cur, Tk::kWord) ? Text(cur) : "";
  }

  // Makes the query so far a subquery named `alias`, if not empty.
  static void Wrap(Select* q, const std::string& alias) {
    Select sub;
    sub.from = "(" + q->Sql() + ")" + (alias.empty() ? "" : " AS " + alias);
    sub.alias = alias;
    *q = std::move(sub);
  }

  // Prepares `q` for setting `clause`: wraps it unless `clause` comes after
  // the clauses already set (repeated FROM/JOINs and WHEREs combine). ORDER BY
  // after a SELECT list (or GROUP BY) would see the SELECT's input rather than
  // its output columns, so it gets a subquery.
  static void Need(Select* q, Clause clause) {
    if (q->last > clause ||
        (q->last == clause && clause != kFrom && clause != kWhere) ||
        (clause == kOrderBy && q->last > kWhere)) {
      Wrap(q, q->alias);
    }
    q->last = clause;
  }

  void CompileStage(size_t b, size_t e, Select* q, Strs* ctes) {
    if (b == e)
      return Fail("empty |> stage");
    std::string stage = Excerpt(b, e);
    auto require = [&](size_t from, const char* what) {
      if (from >= e)
        Fail(std::string(what) + " needs an argument");
      return from < e;
    };
    if (IsKw(b, "WHERE")) {
      if (!require(b + 1, "WHERE"))
        return;
      Need(q, kWhere);
      q->where.push_back(Rewrite(b + 1, e));
    } else if (IsKw(b, "SELECT")) {
      if (!require(b + 1, "SELECT"))
        return;
      Need(q, kSelect);
      q->items = Rewrite(b + 1, e, /*select_list=*/true);
      q->alias.clear();
      if (TopLevel(b + 1, e, [&](size_t i) {
            return Is(i, Tk::kStar);
          }).empty())
        struct_aliases_.clear();
    } else if (IsKw(b, "EXTEND")) {
      if (!require(b + 1, "EXTEND"))
        return;
      Need(q, kSelect);
      q->items = "*, " + Rewrite(b + 1, e, /*select_list=*/true);
    } else if (IsKw(b, "SET")) {
      // `SET col = expr, ...` -> `* REPLACE (expr AS col, ...)`.
      Strs items;
      size_t begin = b + 1;
      std::vector<size_t> commas = TopLevelCommas(begin, e);
      commas.push_back(e);
      for (size_t end : commas) {
        std::vector<size_t> eq =
            TopLevel(begin, end, [&](size_t i) { return Is(i, Tk::kEq); });
        if (eq.empty() || eq[0] != begin + 1 || eq[0] + 1 >= end ||
            !Is(begin, Tk::kWord)) {
          return Fail("SET expects 'column = expression', got '" +
                      Excerpt(begin, end) + "'");
        }
        items.push_back(Rewrite(eq[0] + 1, end) + " AS " + Text(begin));
        begin = end + 1;
      }
      Need(q, kSelect);
      q->items = "* REPLACE (" + Join(items, ", ") + ")";
    } else if (IsKw(b, "DROP")) {
      Strs cols;
      for (size_t i = b + 1; i < e; i += 2) {
        if (!Is(i, Tk::kWord) || (i + 1 < e && !Is(i + 1, Tk::kComma)) ||
            i + 2 == e)
          return Fail("DROP expects a list of column names, got '" + stage +
                      "'");
        cols.push_back(Text(i));
      }
      if (!require(b + 1, "DROP"))
        return;
      Need(q, kSelect);
      q->items = "* EXCLUDE (" + Join(cols, ", ") + ")";
    } else if (IsKw(b, "RENAME")) {
      // `RENAME old [AS] new, ...` -> `* RENAME (old AS new, ...)`.
      Strs items;
      size_t begin = b + 1;
      std::vector<size_t> commas = TopLevelCommas(begin, e);
      commas.push_back(e);
      for (size_t end : commas) {
        size_t n = end - begin;
        bool as = n == 3 && IsKw(begin + 1, "AS");
        if ((n != 2 && !as) || !Is(begin, Tk::kWord) || !Is(end - 1, Tk::kWord))
          return Fail("RENAME expects 'column AS new_name', got '" +
                      Excerpt(begin, end) + "'");
        items.push_back(Text(begin) + " AS " + Text(end - 1));
        begin = end + 1;
      }
      Need(q, kSelect);
      q->items = "* RENAME (" + Join(items, ", ") + ")";
    } else if (IsKw(b, "AGGREGATE")) {
      CompileAggregate(b + 1, e, q);
    } else if (IsKw(b, "ORDER") && IsKw(b + 1, "BY")) {
      if (!require(b + 2, "ORDER BY"))
        return;
      Need(q, kOrderBy);
      q->order_by = Rewrite(b + 2, e);
    } else if (IsKw(b, "LIMIT")) {
      if (!require(b + 1, "LIMIT"))
        return;
      Need(q, kLimit);
      q->limit = Rewrite(b + 1, e);
    } else if (IsKw(b, "AS")) {
      if (b + 2 != e || !Is(b + 1, Tk::kWord))
        return Fail("AS expects a table alias, got '" + stage + "'");
      Wrap(q, Text(b + 1));
    } else if (IsKw(b, "FORK")) {
      // `FORK AS name` names the stream so far, which continues unchanged.
      if (b + 3 != e || !IsKw(b + 1, "AS") || !Is(b + 2, Tk::kWord))
        return Fail("FORK expects 'FORK AS name', got '" + stage + "'");
      std::string name = Text(b + 2);
      ctes->push_back(name + " AS (" + q->Sql() + ")");
      Select fork;
      fork.from = name + (q->alias.empty() ? "" : " AS " + q->alias);
      fork.alias = q->alias;
      *q = std::move(fork);
    } else if (IsKw(b, "INTERVAL") ||
               (IsKw(b, "LEFT") && IsKw(b + 1, "INTERVAL"))) {
      bool left = IsKw(b, "LEFT");
      CompileIntervalStage(b, b + (left ? 2 : 1), e, left, q);
    } else if (IsKw(b, "TREE")) {
      CompileTreeStage(b, b + 1, e, q);
    } else if (IsJoin(b, e)) {
      Need(q, kFrom);
      q->from += " " + Rewrite(b, e);
    } else {
      Fail("unknown |> stage '" + stage +
           "': expected WHERE, SELECT, EXTEND, SET, DROP, RENAME, AGGREGATE, "
           "JOIN, ORDER BY, LIMIT, AS, FORK AS, INTERVAL ... or TREE ...");
    }
  }

  // `[NATURAL|ASOF|POSITIONAL] [INNER|LEFT|RIGHT|FULL|CROSS|SEMI|ANTI]
  // [OUTER] JOIN ...`.
  bool IsJoin(size_t b, size_t e) const {
    static const char* const kPrefixes[] = {
        "NATURAL", "ASOF",  "POSITIONAL", "INNER", "LEFT", "RIGHT",
        "FULL",    "CROSS", "SEMI",       "ANTI",  "OUTER"};
    for (size_t i = b; i < e; i++) {
      if (IsKw(i, "JOIN"))
        return i + 1 < e;
      bool prefix = false;
      for (const char* kw : kPrefixes)
        prefix |= IsKw(i, kw);
      if (!prefix)
        return false;
    }
    return false;
  }

  // `AGGREGATE aggs [GROUP BY key [AS name], ...]`: the keys come first in
  // the output, then the aggregates, as in PerfettoSQL and GoogleSQL.
  void CompileAggregate(size_t b, size_t e, Select* q) {
    std::vector<size_t> group = TopLevel(
        b, e, [&](size_t i) { return IsKw(i, "GROUP") && IsKw(i + 1, "BY"); });
    size_t aggs_end = group.empty() ? e : group[0];
    std::string aggs = Rewrite(b, aggs_end);
    Strs keys;
    Strs exprs;
    if (!group.empty()) {
      size_t begin = group[0] + 2;
      std::vector<size_t> commas = TopLevelCommas(begin, e);
      commas.push_back(e);
      for (size_t end : commas) {
        if (begin >= end)
          return Fail("AGGREGATE ... GROUP BY needs a list of keys");
        keys.push_back(Rewrite(begin, end));
        std::vector<size_t> as =
            TopLevel(begin, end, [&](size_t i) { return IsKw(i, "AS"); });
        exprs.push_back(Rewrite(begin, as.empty() ? end : as.back()));
        begin = end + 1;
      }
    }
    if (aggs.empty() && keys.empty())
      return Fail("AGGREGATE needs aggregates or a GROUP BY");
    if (!aggs.empty())
      keys.push_back(aggs);
    Need(q, kGroupBy);
    q->items = Join(keys, ", ");
    q->group_by = Join(exprs, ", ");
    q->last = kSelect;
    q->alias.clear();
    struct_aliases_.clear();
  }

  // ---------------------------------------------------------------------------
  // Interval, tree and graph operators (docs/intervals.md, trees.md,
  // graphs.md). Each compiles to a call of the pf_* table macros or SQL
  // generators over the query so far, which becomes the next stage's source.
  // ---------------------------------------------------------------------------

  // `INTERVAL (INTERSECTION | UNION) OF (...)`, `INTERVAL[S] FROM EVENTS` or
  // `GRAPH (DFS | BFS | DOMINATOR) TREE` at `b`.
  bool IsOperatorSource(size_t b) const {
    return (IsKw(b, "INTERVAL") &&
            (IsKw(b + 1, "INTERSECTION") || IsKw(b + 1, "UNION")) &&
            IsKw(b + 2, "OF")) ||
           ((IsKw(b, "INTERVAL") || IsKw(b, "INTERVALS")) &&
            IsKw(b + 1, "FROM") && IsKw(b + 2, "EVENTS")) ||
           (IsKw(b, "GRAPH") &&
            (IsKw(b + 1, "DFS") || IsKw(b + 1, "BFS") ||
             IsKw(b + 1, "DOMINATOR")) &&
            IsKw(b + 2, "TREE"));
  }

  // Replaces the query so far with `sql`, keeping the table alias unless
  // `keep_alias` is false.
  static void Replace(Select* q, const std::string& sql, bool keep_alias) {
    std::string alias = keep_alias ? q->alias : "";
    Select next;
    next.from = "(" + sql + ")" + (alias.empty() ? "" : " AS " + alias);
    next.alias = alias;
    *q = std::move(next);
  }

  // A relation argument at `*cur` (up to `e`): `(query)`, `name[.name]` or
  // `function(...)`. Sets `*implicit_alias` to a plain name.
  std::string ParseRelation(size_t* cur,
                            size_t e,
                            std::string* implicit_alias = nullptr) {
    if (implicit_alias)
      implicit_alias->clear();
    if (*cur >= e)
      return "";
    if (Is(*cur, Tk::kOpen) && Text(*cur) == "(") {
      size_t close = Close(*cur);
      std::string inner = CompileQuery(*cur + 1, close);
      *cur = close + 1;
      return "(" + inner + ")";
    }
    size_t start = *cur;
    if (Is(*cur, Tk::kWord)) {
      (*cur)++;
      while (*cur + 1 < e && Is(*cur, Tk::kDot) && Is(*cur + 1, Tk::kWord))
        *cur += 2;
    }
    if (*cur < e && Is(*cur, Tk::kOpen))
      *cur = Close(*cur) + 1;
    if (*cur == start + 1 && Is(start, Tk::kWord) && implicit_alias)
      *implicit_alias = Text(start);
    return Rewrite(start, *cur);
  }

  // `[AS] alias` after a relation, else `fallback`.
  std::string ParseAlias(size_t* cur, size_t e, const std::string& fallback) {
    if (IsKw(*cur, "AS") && *cur + 1 < e) {
      *cur += 2;
      return Text(*cur - 1);
    }
    return fallback;
  }

  // A column list after PER, BY, ORDERED BY or CHARGE: `(c1, c2)` or
  // `c1, c2`.
  Strs ParseColList(size_t* cur, size_t e) {
    Strs cols;
    if (*cur < e && Is(*cur, Tk::kOpen)) {
      size_t close = Close(*cur);
      for (size_t i = *cur + 1; i < close; i++) {
        if (!Is(i, Tk::kComma))
          cols.push_back(Text(i));
      }
      *cur = close + 1;
      return cols;
    }
    while (*cur < e && Is(*cur, Tk::kWord)) {
      cols.push_back(Text((*cur)++));
      if (*cur < e && Is(*cur, Tk::kComma))
        (*cur)++;
      else
        break;
    }
    return cols;
  }

  // `PER [ROLLUP] cols` after the PER keyword.
  Strs ParsePer(size_t* cur, size_t e, bool* rollup = nullptr) {
    bool is_rollup = IsKw(*cur, "ROLLUP") && Is(*cur + 1, Tk::kOpen);
    if (is_rollup)
      (*cur)++;
    if (rollup)
      *rollup = is_rollup;
    return ParseColList(cur, e);
  }

  // The comma-separated items of [b, e), rewritten.
  Strs SplitItems(size_t b, size_t e) {
    Strs items;
    std::vector<size_t> commas = TopLevelCommas(b, e);
    commas.push_back(e);
    for (size_t end : commas) {
      if (end > b)
        items.push_back(Rewrite(b, end));
      b = end + 1;
    }
    return items;
  }

  // Scans the clauses of a stage: for each top-level keyword in `kws` found
  // at the cursor, `on(kw_index, &cur)` consumes its argument; anything else
  // is an error.
  template <typename On>
  bool Clauses(size_t cur,
               size_t e,
               const std::vector<const char*>& kws,
               const std::string& stage,
               On on) {
    while (cur < e && error_.empty()) {
      size_t k = 0;
      while (k < kws.size() && !IsKw(cur, kws[k]))
        k++;
      if (k == kws.size()) {
        Fail("unexpected '" + Excerpt(cur, e) + "' in '" + stage + "'");
        return false;
      }
      cur++;
      on(k, &cur);
    }
    return error_.empty();
  }

  // An operand alias (`AS ts`) that collides with an interval column (`ts`,
  // `dur` or a PER column) is stored as `__pf_sa_<alias>`, and later
  // `<alias>.<field>` references are rewritten to it.
  std::string RegisterStructAlias(const std::string& alias, const Strs& per) {
    std::string low = Lower(alias);
    bool collides = low == "ts" || low == "dur";
    for (const auto& p : per)
      collides |= Lower(p) == low;
    if (!collides) {
      struct_aliases_.erase(low);
      return alias;
    }
    std::string internal = "__pf_sa_" + alias;
    struct_aliases_[low] = internal;
    return internal;
  }

  // Which struct fields `<alias>.<field>` of each INTERVAL INTERSECTION
  // operand the following stages read, so that only those are carried.
  // Returns {} when all may be needed (a SELECT *, FORK, a bare alias, ...).
  std::vector<Strs> NeededStructCols(const Strs& aliases,
                                     const Strs& per,
                                     const std::vector<size_t>& pipes) const {
    size_t scan_end = 0;
    for (size_t s = 0; s + 1 < pipes.size(); s++) {
      size_t sb = pipes[s] + 1;
      size_t se = pipes[s + 1];
      if (sb >= se)
        continue;
      if (IsKw(sb, "FORK") || IsKw(sb, "RENAME") || IsKw(sb, "DROP") ||
          IsKw(sb, "AS"))
        return {};
      bool collapses = IsKw(sb, "AGGREGATE") ||
                       (IsKw(sb, "INTERVAL") &&
                        (IsKw(sb + 1, "FLATTEN") || IsKw(sb + 1, "MERGE")));
      bool select_no_star =
          IsKw(sb, "SELECT") && TopLevel(sb + 1, se, [&](size_t i) {
                                  return Is(i, Tk::kStar);
                                }).empty();
      if (collapses || select_no_star) {
        scan_end = se;
        break;
      }
    }
    if (scan_end == 0)
      return {};
    std::vector<Strs> needed(aliases.size());
    size_t scan_begin = pipes[0] + 1;
    for (size_t idx = 0; idx < aliases.size(); idx++) {
      std::string al = Lower(aliases[idx]);
      bool collides = al == "ts" || al == "dur";
      for (const auto& p : per)
        collides |= Lower(p) == al;
      for (size_t t = scan_begin; t < scan_end; t++) {
        if (!Is(t, Tk::kWord) || Lower(Text(t)) != al ||
            (t > scan_begin && (Is(t - 1, Tk::kDot) || IsKw(t - 1, "AS"))))
          continue;
        if (!Is(t + 1, Tk::kDot)) {
          if (!collides) {
            needed[idx] = {"*"};
            break;
          }
          continue;
        }
        if (!Is(t + 2, Tk::kWord)) {
          needed[idx] = {"*"};
          break;
        }
        std::string col = Text(t + 2);
        bool seen = false;
        for (const auto& c : needed[idx])
          seen |= Lower(c) == Lower(col) || c == "*";
        if (!seen)
          needed[idx].push_back(col);
      }
    }
    return needed;
  }

  // INTERVAL / GRAPH pipeline sources. Returns false if [b, e) is not one.
  bool CompileOperatorSource(size_t b,
                             size_t e,
                             const std::vector<size_t>& pipes,
                             Select* q) {
    std::string stage = Excerpt(b, e);
    // INTERVAL (INTERSECTION | UNION) OF (rel [AS] alias, ...) [PER cols]
    if (IsKw(b, "INTERVAL") && IsKw(b + 2, "OF")) {
      bool is_union = IsKw(b + 1, "UNION");
      size_t cur = b + 3;
      if (!Is(cur, Tk::kOpen) || cur >= e) {
        Fail("expected '(' after '" + Excerpt(b, b + 3) + "'");
        return true;
      }
      size_t close = Close(cur);
      Strs rels, aliases;
      size_t ib = cur + 1;
      std::vector<size_t> commas = TopLevelCommas(ib, close);
      commas.push_back(close);
      for (size_t ie : commas) {
        size_t ic = ib;
        std::string imp;
        rels.push_back(ParseRelation(&ic, ie, &imp));
        std::string al = ParseAlias(&ic, ie, "");
        if (al.empty() && ic + 1 == ie && Is(ic, Tk::kWord))
          al = Text(ic++);
        if (al.empty())
          al = imp.empty() ? "__pf_a" + std::to_string(aliases.size()) : imp;
        if (ic != ie) {
          Fail("unexpected '" + Excerpt(ic, ie) + "' in '" + stage + "'");
          return true;
        }
        aliases.push_back(al);
        ib = ie + 1;
      }
      Strs per;
      Clauses(close + 1, e, {"PER"}, stage,
              [&](size_t, size_t* c) { per = ParsePer(c, e); });
      std::vector<Strs> needed;
      if (!is_union)
        needed = NeededStructCols(aliases, per, pipes);
      for (auto& al : aliases)
        al = RegisterStructAlias(al, per);
      q->from =
          "(" +
          (is_union
               ? "SELECT * FROM pf_interval_union(" + SqlQuoteList(rels) +
                     ", " + SqlQuoteList(aliases) +
                     ", per := " + SqlQuoteList(per) + ")"
               : GenIntervalIntersectionSql(
                     rels, aliases, per, /*full_row=*/true, kThreads, needed)) +
          ")";
      return true;
    }
    // INTERVAL[S] FROM EVENTS rel [AS alias] [PER cols] [STOP rel]
    //   [KEEPING LAST OPEN | CLOSING LAST AT (expr)]
    if (IsKw(b + 1, "FROM") && IsKw(b + 2, "EVENTS")) {
      size_t cur = b + 3;
      std::string rel = ParseRelation(&cur, e);
      ParseAlias(&cur, e, "");
      Strs per;
      std::string stop, close_at;
      bool keep_open = false;
      while (cur < e && error_.empty()) {
        if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePer(&cur, e);
        } else if (IsKw(cur, "STOP")) {
          cur++;
          stop = ParseRelation(&cur, e);
        } else if (IsKw(cur, "KEEPING") && IsKw(cur + 1, "LAST") &&
                   IsKw(cur + 2, "OPEN")) {
          keep_open = true;
          cur += 3;
        } else if (IsKw(cur, "CLOSING") && IsKw(cur + 1, "LAST") &&
                   IsKw(cur + 2, "AT") && cur + 3 < e) {
          cur += 3;
          size_t end = Is(cur, Tk::kOpen) ? Close(cur) + 1 : e;
          close_at = Is(cur, Tk::kOpen) ? Rewrite(cur + 1, end - 1)
                                        : Rewrite(cur, end);
          cur = end;
        } else {
          Fail("unexpected '" + Excerpt(cur, e) + "' in '" + stage + "'");
        }
      }
      std::string sql = "SELECT * FROM pf_intervals_from_events(" +
                        SqlQuote(rel) + ", per := " + SqlQuoteList(per) +
                        ", full_row := true";
      if (!stop.empty())
        sql += ", stop := " + SqlQuote(stop);
      if (keep_open)
        sql += ", keep_last_open := true";
      if (!close_at.empty())
        sql += ", close_last_at := " + SqlQuote(close_at);
      q->from = "(" + sql + "))";
      return true;
    }
    // GRAPH (DFS | BFS | DOMINATOR) TREE NODES rel EDGES rel FROM rel
    //   [PER cols]
    if (IsKw(b, "GRAPH")) {
      std::string nodes, edges, seeds;
      Strs per;
      Clauses(
          b + 3, e, {"NODES", "EDGES", "FROM", "PER"}, stage,
          [&](size_t k, size_t* c) {
            if (k == 3)
              per = ParsePer(c, e);
            else
              (k == 0 ? nodes : k == 1 ? edges : seeds) = ParseRelation(c, e);
          });
      if (nodes.empty() || edges.empty() || seeds.empty()) {
        Fail("'" + stage + "' needs NODES, EDGES and FROM relations");
        return true;
      }
      q->from = "(" +
                GenGraphTreeSql(Lower(Text(b + 1)), nodes, edges, seeds, per,
                                /*full_row=*/true, kThreads) +
                ")";
      return true;
    }
    return false;
  }

  // `[LEFT] INTERVAL ...` stages, with `cur` after INTERVAL.
  void CompileIntervalStage(size_t b,
                            size_t cur,
                            size_t e,
                            bool is_left,
                            Select* q) {
    std::string stage = Excerpt(b, e);
    std::string in = SqlQuote("(" + q->Sql() + ")");
    auto per_only = [&](size_t c, Strs* per) {
      return PerOnly(c, e, stage, per);
    };
    auto unknown = [&]() {
      Fail("unknown INTERVAL stage '" + stage +
           "': expected INTERVAL FLATTEN, MERGE, SPLIT, SUBTRACT, FILL GAPS, "
           "SET BOUNDS, QUANTIZE, JOIN, FIND, KEEP IF or DROP IF");
    };
    if (is_left && !IsKw(cur, "JOIN") && !IsKw(cur, "FIND"))
      return unknown();

    // INTERVAL SPLIT rel [AS alias] [PER cols]
    // INTERVAL SUBTRACT rel [PER cols]
    // INTERVAL FILL GAPS WITH rel [PER cols]
    bool split = IsKw(cur, "SPLIT");
    bool fill =
        IsKw(cur, "FILL") && IsKw(cur + 1, "GAPS") && IsKw(cur + 2, "WITH");
    if (split || IsKw(cur, "SUBTRACT") || fill) {
      cur += fill ? 3 : 1;
      std::string imp;
      std::string rel = ParseRelation(&cur, e, &imp);
      std::string al =
          split ? ParseAlias(&cur, e, imp.empty() ? "right_row" : imp) : "";
      Strs per;
      if (rel.empty())
        return Fail("'" + stage + "' needs a relation");
      if (!per_only(cur, &per))
        return;
      std::string sql =
          split
              ? "SELECT * FROM pf_interval_split(" + in + ", " + SqlQuote(rel) +
                    ", " + SqlQuote(RegisterStructAlias(al, per)) +
                    ", per := " + SqlQuoteList(per) + ")"
          : fill ? "SELECT * FROM pf_interval_fill_gaps(" + in + ", " +
                       SqlQuote(rel) + ", per := " + SqlQuoteList(per) +
                       ", bg_has_per := NULL)"
                 : "SELECT * FROM pf_interval_subtract(" + in + ", " +
                       SqlQuote(rel) + ", per := " + SqlQuoteList(per) + ")";
      return Replace(q, sql, /*keep_alias=*/true);
    }

    // INTERVAL FLATTEN [PER [ROLLUP] cols] [AGGREGATE aggs]
    // INTERVAL MERGE OVERLAPPING [PER [ROLLUP] cols] [AGGREGATE aggs]
    // INTERVAL MERGE CONSECUTIVE BY cols [PER [ROLLUP] cols] [AGGREGATE aggs]
    bool flatten = IsKw(cur, "FLATTEN");
    bool overlapping = IsKw(cur, "MERGE") && IsKw(cur + 1, "OVERLAPPING");
    bool consecutive = IsKw(cur, "MERGE") && IsKw(cur + 1, "CONSECUTIVE");
    if (flatten || overlapping || consecutive) {
      cur += flatten ? 1 : 2;
      Strs per, by, aggs;
      bool rollup = false;
      Clauses(cur, e, {"PER", "BY", "AGGREGATE"}, stage,
              [&](size_t k, size_t* c) {
                if (k == 0) {
                  per = ParsePer(c, e, &rollup);
                } else if (k == 1) {
                  by = ParseColList(c, e);
                } else {
                  aggs = SplitItems(*c, e);
                  *c = e;
                }
              });
      if (!error_.empty())
        return;
      if (consecutive && by.empty())
        return Fail("'" + stage + "' needs BY columns");
      struct_aliases_.clear();
      std::string rel = "(" + q->Sql() + ")";
      auto make = [&](const Strs& p) -> std::string {
        if (flatten)
          return GenIntervalFlattenSql(rel, aggs, p, kThreads);
        std::string fn = overlapping ? "pf_interval_merge_overlapping("
                                     : "pf_interval_merge_consecutive(";
        std::string args =
            SqlQuote(rel) + ", " + (overlapping ? "" : SqlQuoteList(by) + ", ");
        if (aggs.empty()) {
          return "SELECT * EXCLUDE (__pf_cnt) FROM " + fn + args +
                 "'count(*) AS __pf_cnt', per := " + SqlQuoteList(p) + ")";
        }
        return "SELECT * FROM " + fn + args + SqlQuoteList(aggs) +
               ", per := " + SqlQuoteList(p) + ")";
      };
      return Replace(q, rollup ? PerRollup(per, make) : make(per),
                     /*keep_alias=*/false);
    }

    // INTERVAL SET BOUNDS [BEGIN AT] expr (END AT expr | FOR expr)
    //   [PER cols]
    if (IsKw(cur, "SET") && IsKw(cur + 1, "BOUNDS")) {
      cur += 2;
      if (IsKw(cur, "BEGIN") && IsKw(cur + 1, "AT"))
        cur += 2;
      std::vector<size_t> ends = TopLevel(cur, e, [&](size_t i) {
        return (IsKw(i, "END") && IsKw(i + 1, "AT")) || IsKw(i, "FOR");
      });
      if (ends.empty() || ends[0] == cur)
        return Fail("'" + stage +
                    "' needs 'BEGIN AT expr END AT expr' or "
                    "'BEGIN AT expr FOR expr'");
      std::string begin_at = Rewrite(cur, ends[0]);
      bool end_at = IsKw(ends[0], "END");
      size_t second = ends[0] + (end_at ? 2 : 1);
      std::vector<size_t> pers =
          TopLevel(second, e, [&](size_t i) { return IsKw(i, "PER"); });
      size_t second_end = pers.empty() ? e : pers[0];
      Strs per;
      if (second_end == second || !per_only(second_end, &per))
        return Fail("'" + stage + "' needs an end expression");
      return Replace(q,
                     "SELECT * FROM pf_interval_set_bounds(" + in + ", " +
                         SqlQuote(begin_at) + ", " +
                         (end_at ? "end_at := " : "for_dur := ") +
                         SqlQuote(Rewrite(second, second_end)) +
                         ", per := " + SqlQuoteList(per) + ")",
                     /*keep_alias=*/true);
    }

    // INTERVAL QUANTIZE width [ALIGNED TO (TRACE START | ZERO | expr)]
    //   [PER cols]
    if (IsKw(cur, "QUANTIZE")) {
      cur++;
      std::vector<size_t> stops = TopLevel(cur, e, [&](size_t i) {
        return IsKw(i, "ALIGNED") || IsKw(i, "PER");
      });
      size_t width_end = stops.empty() ? e : stops[0];
      if (width_end == cur)
        return Fail("'" + stage + "' needs a bucket width");
      std::string width = Rewrite(cur, width_end);
      std::string aligned = "trace_start";
      cur = width_end;
      if (IsKw(cur, "ALIGNED") && IsKw(cur + 1, "TO")) {
        cur += 2;
        std::vector<size_t> pers =
            TopLevel(cur, e, [&](size_t i) { return IsKw(i, "PER"); });
        size_t end = pers.empty() ? e : pers[0];
        if (IsKw(cur, "TRACE") && IsKw(cur + 1, "START") && cur + 2 == end)
          aligned = "trace_start";
        else if (IsKw(cur, "ZERO") && cur + 1 == end)
          aligned = "zero";
        else
          aligned = Rewrite(cur, end);
        cur = end;
      }
      Strs per;
      if (!per_only(cur, &per))
        return;
      return Replace(q,
                     "SELECT * FROM pf_interval_quantize(" + in + ", " + width +
                         ", aligned_to := " + SqlQuote(aligned) +
                         ", per := " + SqlQuoteList(per) + ")",
                     /*keep_alias=*/true);
    }

    // [LEFT] INTERVAL JOIN rel [AS alias] [relationship] [PER cols]
    // [LEFT] INTERVAL FIND rel [AS alias] (STARTING | ENDING)
    //   (AFTER | BEFORE) (BEGIN | END | BOUNDS) [OF alias] [WITHIN dur]
    //   [PER cols]
    bool join = IsKw(cur, "JOIN");
    if (join || IsKw(cur, "FIND")) {
      cur++;
      std::string imp;
      std::string rel = ParseRelation(&cur, e, &imp);
      if (rel.empty())
        return Fail("'" + stage + "' needs a relation");
      std::string al = ParseAlias(&cur, e, imp.empty() ? "right_row" : imp);
      std::string relationship = "overlapping bounds";
      std::string edge = "starting", dir = "after", anchor = "begin", within;
      Strs per;
      if (join) {
        if (IsRelationship(cur))
          relationship = ParseRelationship(&cur);
      } else {
        if (IsKw(cur, "STARTING") || IsKw(cur, "ENDING"))
          edge = Lower(Text(cur++));
        if (IsKw(cur, "AFTER") || IsKw(cur, "BEFORE"))
          dir = Lower(Text(cur++));
        if (IsKw(cur, "BEGIN") || IsKw(cur, "END") || IsKw(cur, "BOUNDS")) {
          anchor = Lower(Text(cur++));
          if (IsKw(cur, "OF") && cur + 1 < e) {
            std::string of = Text(cur + 1);
            auto it = struct_aliases_.find(Lower(of));
            anchor += " of " + (it == struct_aliases_.end() ? of : it->second);
            cur += 2;
          }
        }
        std::vector<size_t> pers =
            TopLevel(cur, e, [&](size_t i) { return IsKw(i, "PER"); });
        if (IsKw(cur, "WITHIN")) {
          size_t end = pers.empty() ? e : pers[0];
          within = Rewrite(cur + 1, end);
          cur = end;
        }
      }
      if (!per_only(cur, &per))
        return;
      std::string sql_al = RegisterStructAlias(al, per);
      std::string head = join ? "pf_interval_join(" : "pf_interval_find(";
      std::string args =
          in + ", " + SqlQuote(rel) + ", " + SqlQuote(sql_al) + ", " +
          (join ? SqlQuote(relationship)
                : SqlQuote(edge) + ", " + SqlQuote(dir) + ", " +
                      SqlQuote(anchor)) +
          ", per := " + SqlQuoteList(per) +
          ", left_join := " + (is_left ? "true" : "false") +
          (within.empty() ? "" : ", within := " + SqlQuote(within));
      return Replace(q, "SELECT * FROM " + head + args + ")",
                     /*keep_alias=*/true);
    }

    // INTERVAL (KEEP | DROP) IF [relationship] rel [PER cols]
    if ((IsKw(cur, "KEEP") || IsKw(cur, "DROP")) && IsKw(cur + 1, "IF")) {
      bool keep = IsKw(cur, "KEEP");
      cur += 2;
      std::string relationship = "overlapping bounds";
      if (IsRelationship(cur))
        relationship = ParseRelationship(&cur);
      std::string rel = ParseRelation(&cur, e);
      Strs per;
      if (rel.empty())
        return Fail("'" + stage + "' needs a relation");
      if (!per_only(cur, &per))
        return;
      return Replace(
          q,
          std::string("SELECT * FROM ") +
              (keep ? "pf_interval_keep_if(" : "pf_interval_drop_if(") + in +
              ", " + SqlQuote(rel) + ", " + SqlQuote(relationship) +
              ", per := " + SqlQuoteList(per) + ")",
          /*keep_alias=*/true);
    }
    unknown();
  }

  bool IsRelationship(size_t i) const {
    return IsKw(i, "OVERLAPPING") || IsKw(i, "COVERING") || IsKw(i, "WITHIN");
  }

  // OVERLAPPING [BOUNDS] | COVERING (BEGIN | END | BOUNDS) | WITHIN [BOUNDS]
  std::string ParseRelationship(size_t* cur) {
    std::string kind = Lower(Text((*cur)++));
    std::string sub = "bounds";
    if (IsKw(*cur, "BOUNDS") ||
        (kind == "covering" && (IsKw(*cur, "BEGIN") || IsKw(*cur, "END"))))
      sub = Lower(Text((*cur)++));
    return kind + " " + sub;
  }

  // `PER ROLLUP(c1, ..., cn)`: the operator once per prefix of the columns,
  // unioned, with rolled-up columns NULL and grouping_<col> = 1.
  template <typename Make>
  static std::string PerRollup(const Strs& per, Make make) {
    Strs levels;
    for (size_t k = per.size() + 1; k-- > 0;) {
      Strs extra;
      for (size_t j = k; j < per.size(); j++)
        extra.push_back("NULL AS " + SqlIdent(per[j]));
      for (size_t j = 0; j < per.size(); j++)
        extra.push_back(std::string(j < k ? "0" : "1") + " AS grouping_" +
                        per[j]);
      levels.push_back(
          "SELECT *" + (extra.empty() ? "" : ", " + Join(extra, ", ")) +
          " FROM (" + make(Strs(per.begin(), per.begin() + long(k))) + ")");
    }
    return Join(levels, " UNION ALL BY NAME ");
  }

  // `TREE ...` stages, with `cur` after TREE.
  void CompileTreeStage(size_t b, size_t cur, size_t e, Select* q) {
    std::string stage = Excerpt(b, e);
    std::string rel = "(" + q->Sql() + ")";
    std::string in = SqlQuote(rel);
    auto over_sql = [](const std::string& over) {
      return over.empty() ? "" : ", over := " + SqlQuote(over);
    };

    // TREE (KEEP | DROP) IF (ANCESTOR | DESCENDANT) OF rel [OVER rel]
    //   [PER cols]
    // TREE PRUNE AT rel [OVER rel] [PER cols]
    // TREE (CONTRACT | ABSORB) AT rel [OVER rel] [PER cols] AGGREGATE aggs
    // TREE REPARENT [TO NEAREST ANCESTOR IN] rel [OVER rel] [PER cols]
    bool filter =
        (IsKw(cur, "KEEP") || IsKw(cur, "DROP")) && IsKw(cur + 1, "IF");
    bool prune = IsKw(cur, "PRUNE");
    bool collapse = IsKw(cur, "CONTRACT") || IsKw(cur, "ABSORB");
    bool reparent = IsKw(cur, "REPARENT");
    if (filter || prune || collapse || reparent) {
      std::string op = Lower(Text(cur));
      cur += filter ? 2 : 1;
      if (filter) {
        std::string dir = "descendant";
        if (IsKw(cur, "ANCESTOR") || IsKw(cur, "DESCENDANT"))
          dir = Lower(Text(cur++));
        op += "_" + dir;
        if (IsKw(cur, "OF"))
          cur++;
      } else if (reparent) {
        if (IsKw(cur, "TO") && IsKw(cur + 1, "NEAREST") &&
            IsKw(cur + 2, "ANCESTOR") && IsKw(cur + 3, "IN"))
          cur += 4;
      } else if (IsKw(cur, "AT")) {
        cur++;
      }
      std::string marker = ParseRelation(&cur, e);
      if (marker.empty())
        return Fail("'" + stage + "' needs a relation");
      std::string over;
      Strs per, aggs;
      if (!Clauses(cur, e, {"OVER", "PER", "AGGREGATE"}, stage,
                   [&](size_t k, size_t* c) {
                     if (k == 0) {
                       over = ParseRelation(c, e);
                     } else if (k == 1) {
                       per = ParsePer(c, e);
                     } else {
                       aggs = SplitItems(*c, e);
                       *c = e;
                     }
                   }))
        return;
      std::string sql;
      if (filter || prune) {
        sql = GenTreeFilterSql(op, rel, marker, per, over, /*full_row=*/true,
                               kThreads);
      } else if (collapse) {
        sql = "SELECT * FROM pf_tree_" + op + "(" + in + ", " +
              SqlQuote(marker) + ", " + SqlQuoteList(aggs) +
              ", per := " + SqlQuoteList(per) + over_sql(over) +
              ", full_row := true)";
      } else {
        sql = "SELECT * FROM pf_tree_reparent(" + in + ", " + SqlQuote(marker) +
              ", per := " + SqlQuoteList(per) + over_sql(over) + ")";
      }
      return Replace(q, sql, /*keep_alias=*/true);
    }

    // TREE FLATTEN RECURSION BY cols [OVER rel] [PER cols]
    // TREE KEY SIBLINGS BY cols AS (key, parent_key) [ORDERED] [PER cols]
    // TREE MERGE SIBLINGS BY cols [ORDERED] [PER cols] AGGREGATE aggs
    // TREE MERGE INTO PARENT BY cols [INDIRECT] [PER cols] AGGREGATE aggs
    // TREE INVERT [PER cols]
    bool recursion = IsKw(cur, "FLATTEN") && IsKw(cur + 1, "RECURSION");
    bool key = IsKw(cur, "KEY") && IsKw(cur + 1, "SIBLINGS");
    bool siblings = IsKw(cur, "MERGE") && IsKw(cur + 1, "SIBLINGS");
    bool into =
        IsKw(cur, "MERGE") && IsKw(cur + 1, "INTO") && IsKw(cur + 2, "PARENT");
    bool invert = IsKw(cur, "INVERT");
    if (recursion || key || siblings || into || invert) {
      cur += into ? 3 : invert ? 1 : 2;
      Strs by, per, aggs, key_cols = {"node_id", "parent_node_id"};
      std::string over;
      bool flag = false;  // ORDERED or INDIRECT
      if (!Clauses(
              cur, e,
              {"BY", "OVER", "PER", "AGGREGATE", "AS", "ORDERED", "INDIRECT"},
              stage, [&](size_t k, size_t* c) {
                if (k == 0) {
                  by = ParseColList(c, e);
                } else if (k == 1) {
                  over = ParseRelation(c, e);
                } else if (k == 2) {
                  per = ParsePer(c, e);
                } else if (k == 3) {
                  aggs = SplitItems(*c, e);
                  *c = e;
                } else if (k == 4) {
                  key_cols = ParseColList(c, e);
                } else {
                  flag = true;
                }
              }))
        return;
      if (!invert && by.empty())
        return Fail("'" + stage + "' needs BY columns");
      if (key_cols.size() != 2)
        return Fail("'" + stage + "' needs AS (key_column, parent_key_column)");
      std::string flag_sql = flag ? "true" : "false";
      std::string sql =
          recursion ? "SELECT * FROM pf_tree_flatten_recursion(" + in + ", " +
                          SqlQuoteList(by) + ", per := " + SqlQuoteList(per) +
                          over_sql(over) + ")"
          : key ? "SELECT * FROM pf_tree_key_siblings(" + in + ", " +
                      SqlQuoteList(by) + ", " + SqlQuote(key_cols[0]) + ", " +
                      SqlQuote(key_cols[1]) + ", per := " + SqlQuoteList(per) +
                      ", ordered := " + flag_sql + ")"
          : siblings ? "SELECT * FROM pf_tree_merge_siblings(" + in + ", " +
                           SqlQuoteList(by) + ", " + SqlQuoteList(aggs) +
                           ", per := " + SqlQuoteList(per) +
                           ", ordered := " + flag_sql + ")"
          : into     ? "SELECT * FROM pf_tree_merge_into_parent(" + in + ", " +
                           SqlQuoteList(by) + ", " + SqlQuoteList(aggs) +
                           ", per := " + SqlQuoteList(per) +
                           ", indirect := " + flag_sql + ")"
                     : "SELECT * FROM pf_tree_invert(" + in +
                           ", per := " + SqlQuoteList(per) + ")";
      return Replace(q, sql, /*keep_alias=*/true);
    }

    // TREE ACCUMULATE (UP | DOWN) [AGGREGATE] aggs [PER cols], as in
    // PerfettoSQL (`TREE ACCUMULATE UP SUM(dur) AS total`).
    if (IsKw(cur, "ACCUMULATE")) {
      cur++;
      if (!IsKw(cur, "UP") && !IsKw(cur, "DOWN"))
        return Fail("'" + stage + "' needs UP or DOWN");
      std::string dir = Lower(Text(cur++));
      if (IsKw(cur, "AGGREGATE"))
        cur++;
      Strs per;
      std::vector<size_t> pers =
          TopLevel(cur, e, [&](size_t i) { return IsKw(i, "PER"); });
      size_t aggs_end = pers.empty() ? e : pers[0];
      Strs aggs = SplitItems(cur, aggs_end);
      if (aggs.empty())
        return Fail("'" + stage + "' needs aggregates");
      if (!PerOnly(aggs_end, e, stage, &per))
        return;
      return Replace(q,
                     GenTreeAccumulateSql(rel, dir, aggs, per,
                                          /*full_row=*/true, kThreads),
                     /*keep_alias=*/true);
    }

    // TREE EXPAND (UP | DOWN) rel BY cols [ORDERED BY cols]
    //   [CHARGE cols TO (LEAF | ROOT)] [PER cols]
    if (IsKw(cur, "EXPAND")) {
      cur++;
      std::string dir = "down";
      if (IsKw(cur, "UP") || IsKw(cur, "DOWN"))
        dir = Lower(Text(cur++));
      std::string op_rel = ParseRelation(&cur, e);
      if (op_rel.empty())
        return Fail("'" + stage + "' needs a relation");
      Strs by, ordered, charge, per;
      std::string charge_to;
      while (cur < e && error_.empty()) {
        if (IsKw(cur, "BY")) {
          cur++;
          by = ParseColList(&cur, e);
        } else if (IsKw(cur, "ORDERED") && IsKw(cur + 1, "BY")) {
          cur += 2;
          ordered = ParseColList(&cur, e);
        } else if (IsKw(cur, "CHARGE")) {
          cur++;
          charge = ParseColList(&cur, e);
          if (IsKw(cur, "TO") &&
              (IsKw(cur + 1, "LEAF") || IsKw(cur + 1, "ROOT"))) {
            charge_to = Lower(Text(cur + 1));
            cur += 2;
          }
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePer(&cur, e);
        } else {
          Fail("unexpected '" + Excerpt(cur, e) + "' in '" + stage + "'");
        }
      }
      if (!error_.empty())
        return;
      std::string sql = "SELECT * FROM pf_tree_expand(" + in + ", " +
                        SqlQuote(dir) + ", " + SqlQuote(op_rel) + ", " +
                        SqlQuoteList(by) +
                        ", ordered_by := " + SqlQuoteList(ordered) +
                        ", charge := " + SqlQuoteList(charge);
      if (!charge_to.empty())
        sql += ", charge_to := " + SqlQuote(charge_to);
      sql += ", per := " + SqlQuoteList(per) + ")";
      return Replace(q, sql, /*keep_alias=*/true);
    }

    Fail("unknown TREE stage '" + stage +
         "': expected TREE ACCUMULATE, KEEP IF, DROP IF, PRUNE, CONTRACT, "
         "ABSORB, REPARENT, FLATTEN RECURSION, KEY SIBLINGS, MERGE SIBLINGS, "
         "MERGE INTO PARENT, INVERT or EXPAND");
  }

  // An optional `PER cols` clause, and nothing else, in [cur, e).
  bool PerOnly(size_t cur, size_t e, const std::string& stage, Strs* per) {
    return Clauses(cur, e, {"PER"}, stage,
                   [&](size_t, size_t* p) { *per = ParsePer(p, e); });
  }

  std::unordered_map<std::string, std::string> struct_aliases_;

  const std::string& src_;
  std::vector<Tok> toks_;
  std::string error_;
};

void CompilePipeFn(duckdb_function_info info,
                   duckdb_data_chunk input,
                   duckdb_vector output) {
  idx_t rows = duckdb_data_chunk_get_size(input);
  duckdb_vector in = duckdb_data_chunk_get_vector(input, 0);
  auto* strs = static_cast<duckdb_string_t*>(duckdb_vector_get_data(in));
  uint64_t* validity = duckdb_vector_get_validity(in);
  for (idx_t row = 0; row < rows; row++) {
    if (validity && !duckdb_validity_row_is_valid(validity, row)) {
      duckdb_vector_ensure_validity_writable(output);
      duckdb_validity_set_row_invalid(duckdb_vector_get_validity(output), row);
      continue;
    }
    std::string sql(duckdb_string_t_data(&strs[row]),
                    duckdb_string_t_length(strs[row]));
    std::string out;
    std::string error;
    if (!CompilePipe(sql, &out, &error)) {
      duckdb_scalar_function_set_error(info,
                                       ("pipe syntax error: " + error).c_str());
      return;
    }
    duckdb_vector_assign_string_element_len(output, row, out.data(),
                                            out.size());
  }
}

}  // namespace

bool CompilePipe(const std::string& sql, std::string* out, std::string* error) {
  std::vector<Tok> toks;
  return Tokenize(sql, &toks, error) &&
         PipeCompiler(sql, std::move(toks)).CompileScript(out, error);
}

bool RegisterPipeFunctions(duckdb_connection conn) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, "pf_compile_pipe");
  duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_scalar_function_add_parameter(f, t);
  duckdb_scalar_function_set_return_type(f, t);
  duckdb_destroy_logical_type(&t);
  duckdb_scalar_function_set_function(f, CompilePipeFn);
  bool ok = duckdb_register_scalar_function(conn, f) == DuckDBSuccess;
  duckdb_destroy_scalar_function(&f);
  return ok;
}
