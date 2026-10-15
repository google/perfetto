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
// Pipelines may appear wherever a query can: as a statement, a CTE body or a
// parenthesized subquery. Text outside pipelines is copied unchanged, except
// for duration literals (`16ms` -> 16000000). Text without any `|>` is
// returned as is.
#define DUCKDB_EXTENSION_NAME perfetto
#include "duckdb_extension.h"  // no-include-violation-check

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

DUCKDB_EXTENSION_EXTERN

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
    for (const Tok& tok : toks_)
      has_pipe |= tok.kind == Tk::kPipe;
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
    if (!HasTopLevelPipe(cur, e))
      return Rewrite(b, e);
    std::string body = CompilePipeline(cur, e, &ctes);
    return ctes.empty() ? body : with + Join(ctes, ", ") + " " + body;
  }

  // Compiles `source |> stage |> ...` in [b, e). `FORK AS` adds to `ctes`.
  std::string CompilePipeline(size_t b, size_t e, Strs* ctes) {
    std::vector<size_t> pipes =
        TopLevel(b, e, [&](size_t i) { return Is(i, Tk::kPipe); });
    pipes.push_back(e);
    Select q;
    CompileSource(b, pipes[0], &q);
    for (size_t i = 0; i + 1 < pipes.size() && error_.empty(); i++)
      CompileStage(pipes[i] + 1, pipes[i + 1], &q, ctes);
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
    } else if (IsJoin(b, e)) {
      Need(q, kFrom);
      q->from += " " + Rewrite(b, e);
    } else {
      Fail("unknown |> stage '" + stage +
           "': expected WHERE, SELECT, EXTEND, SET, DROP, RENAME, AGGREGATE, "
           "JOIN, ORDER BY, LIMIT, AS or FORK AS");
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
  }

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
    std::vector<Tok> toks;
    if (!Tokenize(sql, &toks, &error) ||
        !PipeCompiler(sql, std::move(toks)).CompileScript(&out, &error)) {
      duckdb_scalar_function_set_error(info,
                                       ("pipe syntax error: " + error).c_str());
      return;
    }
    duckdb_vector_assign_string_element_len(output, row, out.data(),
                                            out.size());
  }
}

}  // namespace

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
