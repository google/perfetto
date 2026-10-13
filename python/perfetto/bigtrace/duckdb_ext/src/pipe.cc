// PerfettoSQL Next (`|>`) pipeline compiler for DuckDB (`docs/spec.md` §§2-7).
//
// Translates PerfettoSQL Next pipelines (`FROM ... |> ...`, `INTERVAL ...`,
// `GRAPH ... TREE ...`, including `FORK AS`, `PER ROLLUP(...)`, nested
// pipelines in CTEs/subqueries, `CREATE [PERFETTO] TABLE/VIEW ... AS`, and
// duration literals like `16ms`) into standard DuckDB SQL backed by the
// native `pf_*` operators.
#include "common.h"

#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Strs = std::vector<std::string>;

enum class TokKind {
  kEof,
  kPipe,    // |>
  kLParen,  // (
  kRParen,  // )
  kComma,   // ,
  kSemi,    // ;
  kEq,      // =
  kStar,    // *
  kDot,     // .
  kWord,    // identifier, keyword, number, or quoted identifier
  kString,  // '...'
  kOther,   // any other operator / punctuation
};

struct Tok {
  TokKind kind = TokKind::kEof;
  size_t start = 0;
  size_t end = 0;
  std::string text;
  std::string upper;  // non-empty only for unquoted words
};

std::string UpperAscii(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

std::string LowerAscii(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

std::string TrimStr(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    b--;
  return s.substr(a, b - a);
}

std::string JoinStrs(const Strs& v, const std::string& sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++) {
    if (i)
      r += sep;
    r += v[i];
  }
  return r;
}

std::string SqlQuote(const std::string& x) {
  std::string q = "'";
  for (char c : x)
    q += (c == '\'') ? std::string("''") : std::string(1, c);
  return q + "'";
}

std::string SqlQuoteList(const Strs& v) {
  if (v.empty())
    return "[]::VARCHAR[]";
  Strs q;
  q.reserve(v.size());
  for (const auto& s : v)
    q.push_back(SqlQuote(s));
  return "[" + JoinStrs(q, ", ") + "]::VARCHAR[]";
}

std::string SqlIdent(const std::string& raw) {
  std::string s = TrimStr(raw);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '`' && s.back() == '`'))) {
    std::string inner = s.substr(1, s.size() - 2);
    std::string q = "\"";
    for (char c : inner)
      q += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
  }
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
      std::string q = "\"";
      for (char ch : s)
        q += (ch == '"') ? std::string("\"\"") : std::string(1, ch);
      return q + "\"";
    }
  }
  return s;
}

std::string SqlError(const std::string& msg) {
  return "SELECT error(" + SqlQuote(msg) + ")";
}

// Checks if an unquoted word is a PerfettoSQL Next duration literal such as
// `100ns`, `50us`, `16ms`, `1.5s`, `2m`, `1h`, `1d`. If so, returns the
// integer nanosecond literal string; otherwise returns "".
std::string TryParseDurationLiteral(const std::string& w) {
  if (w.empty() || !std::isdigit(static_cast<unsigned char>(w[0])))
    return "";
  size_t i = 0;
  bool seen_dot = false;
  while (i < w.size() &&
         (std::isdigit(static_cast<unsigned char>(w[i])) || w[i] == '.')) {
    if (w[i] == '.') {
      if (seen_dot)
        return "";
      seen_dot = true;
    }
    i++;
  }
  if (i == 0 || i == w.size())
    return "";
  std::string unit = LowerAscii(w.substr(i));
  int64_t mult = 0;
  if (unit == "ns") {
    mult = 1LL;
  } else if (unit == "us") {
    mult = 1000LL;
  } else if (unit == "ms") {
    mult = 1000000LL;
  } else if (unit == "s") {
    mult = 1000000000LL;
  } else if (unit == "m") {
    mult = 60LL * 1000000000LL;
  } else if (unit == "h") {
    mult = 3600LL * 1000000000LL;
  } else if (unit == "d") {
    mult = 86400LL * 1000000000LL;
  } else {
    return "";
  }
  std::string num_str = w.substr(0, i);
  if (!seen_dot) {
    int64_t v = std::stoll(num_str);
    return std::to_string(v * mult);
  }
  double dv = std::stod(num_str);
  return std::to_string(static_cast<int64_t>(std::llround(dv * mult)));
}

std::vector<Tok> Tokenize(const std::string& s) {
  std::vector<Tok> out;
  size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    if (std::isspace(ch)) {
      i++;
      continue;
    }
    // Line comment
    if (s[i] == '-' && i + 1 < n && s[i + 1] == '-') {
      i += 2;
      while (i < n && s[i] != '\n')
        i++;
      continue;
    }
    // Block comment
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
        i++;
      if (i + 1 < n)
        i += 2;
      continue;
    }
    // Pipe operator |>
    if (s[i] == '|' && i + 1 < n && s[i + 1] == '>') {
      out.push_back({TokKind::kPipe, i, i + 2, "|>", ""});
      i += 2;
      continue;
    }
    // Single-quoted string
    if (s[i] == '\'') {
      size_t start = i++;
      while (i < n) {
        if (s[i] == '\'') {
          i++;
          if (i < n && s[i] == '\'') {
            i++;
            continue;
          }
          break;
        }
        i++;
      }
      out.push_back(
          {TokKind::kString, start, i, s.substr(start, i - start), ""});
      continue;
    }
    // Double-quoted or backtick-quoted identifier
    if (s[i] == '"' || s[i] == '`') {
      char q = s[i];
      size_t start = i++;
      while (i < n) {
        if (s[i] == q) {
          i++;
          if (i < n && s[i] == q) {
            i++;
            continue;
          }
          break;
        }
        i++;
      }
      out.push_back({TokKind::kWord, start, i, s.substr(start, i - start), ""});
      continue;
    }
    if (s[i] == '(') {
      out.push_back({TokKind::kLParen, i, i + 1, "(", ""});
      i++;
      continue;
    }
    if (s[i] == ')') {
      out.push_back({TokKind::kRParen, i, i + 1, ")", ""});
      i++;
      continue;
    }
    if (s[i] == ',') {
      out.push_back({TokKind::kComma, i, i + 1, ",", ""});
      i++;
      continue;
    }
    if (s[i] == ';') {
      out.push_back({TokKind::kSemi, i, i + 1, ";", ""});
      i++;
      continue;
    }
    if (s[i] == '=') {
      out.push_back({TokKind::kEq, i, i + 1, "=", ""});
      i++;
      continue;
    }
    if (s[i] == '*') {
      out.push_back({TokKind::kStar, i, i + 1, "*", ""});
      i++;
      continue;
    }
    if (s[i] == '.') {
      out.push_back({TokKind::kDot, i, i + 1, ".", ""});
      i++;
      continue;
    }
    // Word / number (including decimal inside number like 1.5s)
    if (std::isalnum(ch) || s[i] == '_' || s[i] == '$') {
      size_t start = i;
      bool is_num = std::isdigit(ch) != 0;
      while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isalnum(c) || s[i] == '_' || s[i] == '$') {
          i++;
        } else if (is_num && s[i] == '.' && i + 1 < n &&
                   std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
          i++;
        } else {
          break;
        }
      }
      std::string txt = s.substr(start, i - start);
      out.push_back({TokKind::kWord, start, i, txt, UpperAscii(txt)});
      continue;
    }
    // Multi-char operators (::, <=, >=, <>, !=, ||, :=, ->, ->>)
    size_t start = i++;
    if (i < n) {
      std::string two = s.substr(start, 2);
      if (two == "::" || two == "<=" || two == ">=" || two == "<>" ||
          two == "!=" || two == "||" || two == ":=" || two == "->") {
        i++;
      }
    }
    out.push_back({TokKind::kOther, start, i, s.substr(start, i - start), ""});
  }
  out.push_back({TokKind::kEof, n, n, "", ""});
  return out;
}

class PipeCompiler {
 public:
  PipeCompiler(const std::string& src,
               std::vector<Tok> toks,
               const Strs& default_per,
               int64_t threads,
               int* fork_counter)
      : src_(src),
        toks_(std::move(toks)),
        default_per_(default_per),
        threads_(threads),
        fork_counter_(fork_counter) {}

  std::string CompileScript(size_t begin, size_t end) {
    Strs stmts;
    size_t cur = begin;
    int depth = 0;
    for (size_t i = begin; i < end; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
        depth--;
      } else if (toks_[i].kind == TokKind::kSemi && depth == 0) {
        if (i > cur) {
          std::string st = CompileSingleStmt(cur, i);
          if (!st.empty())
            stmts.push_back(st);
        }
        cur = i + 1;
      }
    }
    if (cur < end) {
      std::string st = CompileSingleStmt(cur, end);
      if (!st.empty())
        stmts.push_back(st);
    }
    if (stmts.empty())
      return "SELECT 1 WHERE false";
    return JoinStrs(stmts, ";\n");
  }

 private:
  bool IsKw(size_t idx, const char* kw) const {
    return idx < toks_.size() && toks_[idx].upper == kw;
  }

  // Finds the matching ')' for '(' at `open_idx` within `[open_idx, end)`.
  size_t FindMatchingRParen(size_t open_idx, size_t end) const {
    int depth = 0;
    for (size_t i = open_idx; i < end; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen) {
        depth--;
        if (depth == 0)
          return i;
      }
    }
    return end;
  }

  // True if token range `[b, e)` has a top-level `|>` or starts with a
  // PerfettoSQL Next pipeline source keyword (`FROM`, `INTERVAL`, `INTERVALS`,
  // `GRAPH`).
  bool RangeIsPipeline(size_t b, size_t e) const {
    if (b >= e)
      return false;
    if (IsKw(b, "FROM") || IsKw(b, "INTERVAL") || IsKw(b, "INTERVALS") ||
        IsKw(b, "GRAPH")) {
      return true;
    }
    int depth = 0;
    for (size_t i = b; i < e; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
        depth--;
      } else if (toks_[i].kind == TokKind::kPipe && depth == 0) {
        return true;
      }
    }
    return false;
  }

  // True if token range `[b, e)` inside parentheses is a subquery or pipeline.
  bool RangeIsSubqueryOrPipe(size_t b, size_t e) const {
    if (b >= e)
      return false;
    if (RangeIsPipeline(b, e))
      return true;
    return IsKw(b, "SELECT") || IsKw(b, "WITH") || IsKw(b, "VALUES");
  }

  // Rewrites a token range `[b, e)` that is not itself a top-level pipeline,
  // recursively compiling any parenthesized subqueries/pipelines, converting
  // duration literals (`16ms` -> `16000000`), and optionally converting
  // `* EXCEPT (` to `* EXCLUDE (` and `@input` to `input_replacement`.
  std::string RewriteTokens(size_t b,
                            size_t e,
                            bool star_except_to_exclude = false,
                            const std::string& input_replacement = "") {
    std::string out;
    size_t last_end = (b < e) ? toks_[b].start : 0;
    for (size_t i = b; i < e; i++) {
      if (toks_[i].start > last_end)
        out += src_.substr(last_end, toks_[i].start - last_end);

      // @input replacement inside SQL(...) stage
      if (!input_replacement.empty() && toks_[i].kind == TokKind::kOther &&
          toks_[i].text == "@" && i + 1 < e &&
          toks_[i + 1].start == toks_[i].end && IsKw(i + 1, "INPUT")) {
        out += input_replacement;
        i++;
        last_end = toks_[i].end;
        continue;
      }

      // `* EXCEPT (` -> `* EXCLUDE (`
      if (star_except_to_exclude && IsKw(i, "EXCEPT") && i > b &&
          toks_[i - 1].kind == TokKind::kStar && i + 1 < e &&
          toks_[i + 1].kind == TokKind::kLParen) {
        out += "EXCLUDE";
        last_end = toks_[i].end;
        continue;
      }

      // Parenthesized group: check if it is a subquery or pipeline
      if (toks_[i].kind == TokKind::kLParen) {
        size_t close = FindMatchingRParen(i, e);
        if (close < e) {
          if (RangeIsSubqueryOrPipe(i + 1, close)) {
            out += "(" + CompileQueryExpr(i + 1, close) + ")";
          } else {
            out += "(" +
                   RewriteTokens(i + 1, close, star_except_to_exclude,
                                 input_replacement) +
                   ")";
          }
          last_end = toks_[close].end;
          i = close;
          continue;
        }
      }

      // Duration literal (`16ms`, `1.5s`, `500us`, `100ns`, etc.)
      if (toks_[i].kind == TokKind::kWord && !toks_[i].upper.empty()) {
        std::string dur = TryParseDurationLiteral(toks_[i].text);
        if (!dur.empty()) {
          out += dur;
          last_end = toks_[i].end;
          continue;
        }
      }

      // Disambiguate struct aliases (`AS ts`, `AS dur`, etc.) when followed by
      // `.`
      if (toks_[i].kind == TokKind::kWord && !struct_aliases_.empty() &&
          i + 1 < e && toks_[i + 1].kind == TokKind::kDot &&
          (i == b || toks_[i - 1].kind != TokKind::kDot)) {
        auto it = struct_aliases_.find(LowerAscii(toks_[i].text));
        if (it != struct_aliases_.end()) {
          out += SqlIdent(it->second);
          last_end = toks_[i].end;
          continue;
        }
      }

      out += toks_[i].text;
      last_end = toks_[i].end;
    }
    return TrimStr(out);
  }

  // Compiles a single top-level statement (`[b, e)` without `;`).
  std::string CompileSingleStmt(size_t b, size_t e) {
    if (b >= e)
      return "";
    // Strip `PERFETTO PRAGMA ...` or `INCLUDE PERFETTO MODULE ...`
    if ((IsKw(b, "PERFETTO") && IsKw(b + 1, "PRAGMA")) ||
        (IsKw(b, "INCLUDE") && IsKw(b + 1, "PERFETTO") &&
         IsKw(b + 2, "MODULE"))) {
      return "";
    }
    // EXPLAIN [ANALYZE] <stmt>
    if (IsKw(b, "EXPLAIN")) {
      size_t cur = b + 1;
      std::string prefix = "EXPLAIN ";
      if (IsKw(cur, "ANALYZE")) {
        prefix += "ANALYZE ";
        cur++;
      }
      return prefix + CompileSingleStmt(cur, e);
    }
    // CREATE [OR REPLACE] [TEMP|TEMPORARY] [PERFETTO] (TABLE|VIEW) name
    // [(schema)] AS <query>
    if (IsKw(b, "CREATE")) {
      size_t cur = b + 1;
      std::string prefix = "CREATE ";
      if (IsKw(cur, "OR") && IsKw(cur + 1, "REPLACE")) {
        prefix += "OR REPLACE ";
        cur += 2;
      }
      if (IsKw(cur, "TEMP") || IsKw(cur, "TEMPORARY")) {
        prefix += "TEMP ";
        cur++;
      }
      bool is_perfetto = false;
      if (IsKw(cur, "PERFETTO")) {
        is_perfetto = true;
        cur++;
      }
      if (IsKw(cur, "TABLE") || IsKw(cur, "VIEW")) {
        std::string kind = toks_[cur].upper;
        cur++;
        if (IsKw(cur, "IF") && IsKw(cur + 1, "NOT") &&
            IsKw(cur + 2, "EXISTS")) {
          kind += " IF NOT EXISTS";
          cur += 3;
        }
        // Table / view name (possibly schema.name)
        size_t name_start = cur;
        if (cur < e && toks_[cur].kind == TokKind::kWord) {
          cur++;
          while (cur + 1 < e && toks_[cur].kind == TokKind::kDot &&
                 toks_[cur + 1].kind == TokKind::kWord) {
            cur += 2;
          }
        }
        std::string name = RewriteTokens(name_start, cur);
        // Optional (schema) or (col_list) before AS
        std::string col_list;
        if (cur < e && toks_[cur].kind == TokKind::kLParen) {
          size_t close = FindMatchingRParen(cur, e);
          if (close < e) {
            if (!is_perfetto)
              col_list = " (" + RewriteTokens(cur + 1, close) + ")";
            cur = close + 1;
          }
        }
        if (IsKw(cur, "AS")) {
          cur++;
          return prefix + kind + " " + name + col_list + " AS " +
                 CompileQueryExpr(cur, e);
        }
      }
    }
    return CompileQueryExpr(b, e);
  }

  // Compiles a query expression `[b, e)`, which may start with `WITH ...`
  // followed by a pipeline or standard `SELECT`, or be a pipeline directly.
  std::string CompileQueryExpr(size_t b, size_t e) {
    if (b >= e)
      return "";
    // Unwrap outer redundant parens if the whole `[b, e)` is `( ... )` and
    // nothing follows the matching `)`.
    if (toks_[b].kind == TokKind::kLParen) {
      size_t close = FindMatchingRParen(b, e);
      if (close == e - 1)
        return CompileQueryExpr(b + 1, close);
    }

    // Handle leading WITH [RECURSIVE] cte1 AS (...), cte2 AS (...) <main_query>
    if (IsKw(b, "WITH")) {
      size_t cur = b + 1;
      std::string with_hdr = "WITH ";
      if (IsKw(cur, "RECURSIVE")) {
        with_hdr += "RECURSIVE ";
        cur++;
      }
      Strs ctes;
      while (cur < e) {
        size_t name_start = cur;
        if (toks_[cur].kind != TokKind::kWord)
          break;
        cur++;
        // Optional CTE column list: cte(a, b)
        if (cur < e && toks_[cur].kind == TokKind::kLParen) {
          size_t close = FindMatchingRParen(cur, e);
          if (close < e)
            cur = close + 1;
        }
        std::string cte_head = RewriteTokens(name_start, cur);
        if (!IsKw(cur, "AS"))
          break;
        cur++;
        std::string mat;
        if (IsKw(cur, "MATERIALIZED")) {
          mat = "MATERIALIZED ";
          cur++;
        } else if (IsKw(cur, "NOT") && IsKw(cur + 1, "MATERIALIZED")) {
          mat = "NOT MATERIALIZED ";
          cur += 2;
        }
        if (cur >= e || toks_[cur].kind != TokKind::kLParen)
          break;
        size_t close = FindMatchingRParen(cur, e);
        if (close >= e)
          break;
        std::string cte_body = CompileQueryExpr(cur + 1, close);
        ctes.push_back(cte_head + " AS " + mat + "(" + cte_body + ")");
        cur = close + 1;
        if (cur < e && toks_[cur].kind == TokKind::kComma) {
          cur++;
        } else {
          break;
        }
      }
      if (!ctes.empty() && cur < e) {
        Strs fork_ctes;
        std::string main_sql = CompilePipelineOrSql(cur, e, &fork_ctes);
        for (const auto& fc : fork_ctes)
          ctes.push_back(fc);
        return with_hdr + JoinStrs(ctes, ", ") + " " + main_sql;
      }
    }

    Strs fork_ctes;
    std::string body = CompilePipelineOrSql(b, e, &fork_ctes);
    if (!fork_ctes.empty())
      return "WITH " + JoinStrs(fork_ctes, ", ") + " " + body;
    return body;
  }

  // Compiles `[b, e)` where `WITH` has already been stripped.
  std::string CompilePipelineOrSql(size_t b, size_t e, Strs* fork_ctes) {
    auto saved_aliases = struct_aliases_;
    struct_aliases_.clear();
    std::string res = CompilePipelineOrSqlImpl(b, e, fork_ctes);
    struct_aliases_ = std::move(saved_aliases);
    return res;
  }

  std::string CompilePipelineOrSqlImpl(size_t b, size_t e, Strs* fork_ctes) {
    if (!RangeIsPipeline(b, e))
      return RewriteTokens(b, e);

    // Split `[b, e)` on top-level `|>` tokens into [stage_0, stage_1, ...].
    std::vector<std::pair<size_t, size_t>> stages;
    size_t start = b;
    int depth = 0;
    for (size_t i = b; i < e; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
        depth--;
      } else if (toks_[i].kind == TokKind::kPipe && depth == 0) {
        stages.push_back({start, i});
        start = i + 1;
      }
    }
    stages.push_back({start, e});

    std::string cur_alias;
    std::string cur_sql =
        CompileSource(stages[0].first, stages[0].second, &cur_alias, stages);
    for (size_t s = 1; s < stages.size(); s++) {
      cur_sql = CompileStage(cur_sql, stages[s].first, stages[s].second,
                             &cur_alias, fork_ctes);
    }
    return cur_sql;
  }

  // Infers which struct fields `<al>.<field>` of each INTERVAL INTERSECTION OF
  // operand are referenced across subsequent pipeline stages `stages[1..]`.
  // Returns `{}` if all fields are needed (e.g., no narrowing stage, `FORK`,
  // `SQL(...)`, or whole-struct / `.*` reference), or a vector of length
  // `orig_aliases.size()` listing the referenced field names per operand.
  std::vector<Strs> InferNeededStructCols(
      const Strs& orig_aliases,
      const Strs& per,
      const std::vector<std::pair<size_t, size_t>>& stages) const {
    if (stages.size() <= 1)
      return {};
    size_t scan_end = 0;
    for (size_t s = 1; s < stages.size(); s++) {
      size_t sb = stages[s].first;
      size_t se = stages[s].second;
      if (sb >= se)
        continue;
      if (IsKw(sb, "FORK") || IsKw(sb, "SQL") || IsKw(sb, "UNION") ||
          IsKw(sb, "INTERSECT") || IsKw(sb, "EXCEPT") || IsKw(sb, "RENAME") ||
          IsKw(sb, "DROP")) {
        return {};
      }
      if (IsKw(sb, "AGGREGATE") ||
          (IsKw(sb, "INTERVAL") &&
           (IsKw(sb + 1, "FLATTEN") || IsKw(sb + 1, "MERGE")))) {
        scan_end = se;
        break;
      }
      if (IsKw(sb, "SELECT")) {
        bool has_star = false;
        size_t cur = sb + 1;
        if (IsKw(cur, "DISTINCT"))
          cur++;
        for (size_t i = cur; i < se; i++) {
          if (toks_[i].kind == TokKind::kStar) {
            has_star = true;
            break;
          }
        }
        if (!has_star) {
          scan_end = se;
          break;
        }
      }
    }
    if (scan_end == 0)
      return {};

    size_t n = orig_aliases.size();
    std::vector<Strs> needed(n);
    size_t scan_begin = stages[1].first;
    for (size_t idx = 0; idx < n; idx++) {
      std::string low_al = LowerAscii(orig_aliases[idx]);
      bool collides = (low_al == "ts" || low_al == "dur");
      if (!collides) {
        for (const auto& p : per) {
          if (LowerAscii(p) == low_al) {
            collides = true;
            break;
          }
        }
      }
      for (size_t t = scan_begin; t < scan_end; t++) {
        if (toks_[t].kind != TokKind::kWord ||
            LowerAscii(toks_[t].text) != low_al) {
          continue;
        }
        if (t > scan_begin && toks_[t - 1].kind == TokKind::kDot)
          continue;
        if (t > scan_begin && IsKw(t - 1, "AS"))
          continue;
        if (t + 1 < scan_end && toks_[t + 1].kind == TokKind::kDot) {
          if (t + 2 < scan_end && toks_[t + 2].kind == TokKind::kWord) {
            std::string col = toks_[t + 2].text;
            std::string low_col = LowerAscii(col);
            bool exists = false;
            for (const auto& existing : needed[idx]) {
              if (LowerAscii(existing) == low_col || existing == "*") {
                exists = true;
                break;
              }
            }
            if (!exists)
              needed[idx].push_back(col);
          } else {
            needed[idx] = {"*"};
            break;
          }
        } else if (!collides) {
          needed[idx] = {"*"};
          break;
        }
      }
    }
    return needed;
  }

  // Registers an operand struct alias (`AS ts`, `AS dur`, etc.). When the
  // alias name collides with an interval column (`ts`, `dur`, or a `per`
  // column), returns an internal SQL column name `__pf_sa_<al>` and records
  // the mapping so `<al>.<field>` rewrites to `"__pf_sa_<al>".<field>` while
  // standalone `<al>` still refers to the interval column (`ts`, `dur`, etc.).
  std::string RegisterStructAlias(const std::string& al, const Strs& per) {
    std::string low = LowerAscii(al);
    bool collides = (low == "ts" || low == "dur");
    if (!collides) {
      for (const auto& p : per) {
        if (LowerAscii(p) == low) {
          collides = true;
          break;
        }
      }
    }
    if (collides) {
      std::string internal = "__pf_sa_" + al;
      struct_aliases_[low] = internal;
      return internal;
    }
    struct_aliases_.erase(low);
    return al;
  }

  // Helper: wraps `cur_sql` as a FROM target, attaching `cur_alias` if
  // non-empty.
  static std::string FromTarget(const std::string& cur_sql,
                                const std::string& cur_alias) {
    std::string sub = "(" + cur_sql + ")";
    if (!cur_alias.empty())
      sub += " AS " + SqlIdent(cur_alias);
    return sub;
  }

  // Parses a relation argument starting at `*cur` (up to `e`):
  // - `( <subquery_or_pipeline> )` -> returns `(<compiled>)`
  // - `schema.table` or `table_func(...)` -> returns `<compiled_rel>`
  // Also sets `*implicit_alias` if the relation is a plain identifier.
  std::string ParseRelation(size_t* cur,
                            size_t e,
                            std::string* implicit_alias = nullptr) {
    if (implicit_alias)
      implicit_alias->clear();
    if (*cur >= e)
      return "";
    if (toks_[*cur].kind == TokKind::kLParen) {
      size_t close = FindMatchingRParen(*cur, e);
      if (close < e) {
        std::string inner = CompileQueryExpr(*cur + 1, close);
        *cur = close + 1;
        return "(" + inner + ")";
      }
    }
    size_t start = *cur;
    // Consume word [. word]* [ ( ... ) ]
    if (*cur < e && toks_[*cur].kind == TokKind::kWord) {
      (*cur)++;
      while (*cur + 1 < e && toks_[*cur].kind == TokKind::kDot &&
             toks_[*cur + 1].kind == TokKind::kWord) {
        *cur += 2;
      }
    }
    if (*cur < e && toks_[*cur].kind == TokKind::kLParen) {
      size_t close = FindMatchingRParen(*cur, e);
      if (close < e)
        *cur = close + 1;
    }
    if (*cur == start + 1 && toks_[start].kind == TokKind::kWord &&
        implicit_alias) {
      *implicit_alias = toks_[start].text;
    }
    return RewriteTokens(start, *cur);
  }

  // Parses a column list after `PER`, `BY`, `ORDERED BY`, or `CHARGE`:
  // Accepts either `(c1, c2, ...)` or `c1, c2, ...`.
  Strs ParseColList(size_t* cur, size_t e) {
    Strs cols;
    if (*cur >= e)
      return cols;
    if (toks_[*cur].kind == TokKind::kLParen) {
      size_t close = FindMatchingRParen(*cur, e);
      if (close < e) {
        size_t i = *cur + 1;
        while (i < close) {
          if (toks_[i].kind == TokKind::kComma) {
            i++;
            continue;
          }
          cols.push_back(toks_[i].text);
          i++;
        }
        *cur = close + 1;
        return cols;
      }
    }
    while (*cur < e && toks_[*cur].kind == TokKind::kWord) {
      cols.push_back(toks_[*cur].text);
      (*cur)++;
      if (*cur < e && toks_[*cur].kind == TokKind::kComma) {
        (*cur)++;
      } else {
        break;
      }
    }
    return cols;
  }

  // Parses `PER [ROLLUP (] cols [)]`. Sets `*is_rollup = true` if ROLLUP was
  // specified.
  Strs ParsePerClause(size_t* cur, size_t e, bool* is_rollup = nullptr) {
    if (is_rollup)
      *is_rollup = false;
    if (IsKw(*cur, "ROLLUP") && *cur + 1 < e &&
        toks_[*cur + 1].kind == TokKind::kLParen) {
      if (is_rollup)
        *is_rollup = true;
      (*cur)++;
    }
    return ParseColList(cur, e);
  }

  // Resolves effective `per` columns: uses explicit `per` if non-empty, else
  // `default_per_`.
  Strs EffectivePer(const Strs& explicit_per) const {
    return explicit_per.empty() ? default_per_ : explicit_per;
  }

  // Splits a token range `[b, e)` on top-level commas into trimmed expression
  // strings (with subqueries/duration literals rewritten).
  Strs SplitCommaItems(size_t b,
                       size_t e,
                       bool star_except_to_exclude = false) {
    Strs items;
    size_t start = b;
    int depth = 0;
    for (size_t i = b; i < e; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
        depth--;
      } else if (toks_[i].kind == TokKind::kComma && depth == 0) {
        if (i > start)
          items.push_back(RewriteTokens(start, i, star_except_to_exclude));
        start = i + 1;
      }
    }
    if (start < e)
      items.push_back(RewriteTokens(start, e, star_except_to_exclude));
    return items;
  }

  // Splits a token range `[b, e)` on top-level commas into token index pairs.
  std::vector<std::pair<size_t, size_t>> SplitCommaRanges(size_t b,
                                                          size_t e) const {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t start = b;
    int depth = 0;
    for (size_t i = b; i < e; i++) {
      if (toks_[i].kind == TokKind::kLParen) {
        depth++;
      } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
        depth--;
      } else if (toks_[i].kind == TokKind::kComma && depth == 0) {
        if (i > start)
          ranges.push_back({start, i});
        start = i + 1;
      }
    }
    if (start < e)
      ranges.push_back({start, e});
    return ranges;
  }

  // -------------------------------------------------------------------------
  // Pipeline Source Compilation (§2.1, §5.1, §7.1)
  // -------------------------------------------------------------------------
  std::string CompileSource(
      size_t b,
      size_t e,
      std::string* cur_alias,
      const std::vector<std::pair<size_t, size_t>>& stages) {
    cur_alias->clear();
    if (b >= e)
      return "";

    // 1. `FROM <rel> [[AS] <alias>]`
    if (IsKw(b, "FROM")) {
      size_t cur = b + 1;
      std::string imp;
      std::string rel = ParseRelation(&cur, e, &imp);
      if (cur < e && IsKw(cur, "AS") && cur + 1 < e) {
        *cur_alias = toks_[cur + 1].text;
      } else if (cur < e && toks_[cur].kind == TokKind::kWord) {
        *cur_alias = toks_[cur].text;
      }
      if (!cur_alias->empty())
        return "SELECT * FROM " + rel + " AS " + SqlIdent(*cur_alias);
      return "SELECT * FROM " + rel;
    }

    // 2. `INTERVAL (INTERSECTION | UNION) OF (<rel> [AS <alias>], ...) [PER
    // ...]`
    if (IsKw(b, "INTERVAL") &&
        (IsKw(b + 1, "INTERSECTION") || IsKw(b + 1, "UNION")) &&
        IsKw(b + 2, "OF")) {
      bool is_union = IsKw(b + 1, "UNION");
      size_t cur = b + 3;
      if (cur >= e || toks_[cur].kind != TokKind::kLParen)
        return SqlError("INTERVAL INTERSECTION/UNION OF: expected '('");
      size_t close = FindMatchingRParen(cur, e);
      if (close >= e)
        return SqlError("INTERVAL INTERSECTION/UNION OF: unclosed '('");
      auto items = SplitCommaRanges(cur + 1, close);
      Strs rels, aliases;
      for (size_t idx = 0; idx < items.size(); idx++) {
        size_t ic = items[idx].first;
        size_t ie = items[idx].second;
        std::string imp;
        std::string r = ParseRelation(&ic, ie, &imp);
        std::string al;
        if (ic < ie && IsKw(ic, "AS") && ic + 1 < ie) {
          al = toks_[ic + 1].text;
        } else if (ic < ie && toks_[ic].kind == TokKind::kWord) {
          al = toks_[ic].text;
        } else if (!imp.empty()) {
          al = imp;
        } else {
          al = "__pf_a" + std::to_string(idx);
        }
        rels.push_back(r);
        aliases.push_back(al);
      }
      cur = close + 1;
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      std::vector<Strs> needed_cols;
      if (!is_union) {
        needed_cols = InferNeededStructCols(aliases, per, stages);
      }
      for (auto& al : aliases) {
        al = RegisterStructAlias(al, per);
      }
      if (!is_union) {
        return GenIntervalIntersectionSql(rels, aliases, per, /*full_row=*/true,
                                          threads_, needed_cols);
      }
      return "SELECT * FROM pf_interval_union(" + SqlQuoteList(rels) + ", " +
             SqlQuoteList(aliases) + ", per := " + SqlQuoteList(per) + ")";
    }

    // 3. `INTERVALS FROM EVENTS <rel> [PER <per>] [STOP <stop_rel>]
    //     [KEEPING LAST OPEN | CLOSING LAST AT (<expr>)]`
    if ((IsKw(b, "INTERVALS") && IsKw(b + 1, "FROM") &&
         IsKw(b + 2, "EVENTS")) ||
        (IsKw(b, "INTERVAL") && IsKw(b + 1, "FROM") && IsKw(b + 2, "EVENTS"))) {
      size_t cur = b + 3;
      std::string rel = ParseRelation(&cur, e);
      if (IsKw(cur, "AS") && cur + 1 < e)
        cur += 2;
      Strs per;
      std::string stop_rel;
      bool keep_open = false;
      std::string close_at;
      while (cur < e) {
        if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else if (IsKw(cur, "STOP")) {
          cur++;
          stop_rel = ParseRelation(&cur, e);
        } else if (IsKw(cur, "KEEPING") && IsKw(cur + 1, "LAST") &&
                   IsKw(cur + 2, "OPEN")) {
          keep_open = true;
          cur += 3;
        } else if (IsKw(cur, "CLOSING") && IsKw(cur + 1, "LAST") &&
                   IsKw(cur + 2, "AT")) {
          cur += 3;
          if (cur < e && toks_[cur].kind == TokKind::kLParen) {
            size_t close = FindMatchingRParen(cur, e);
            close_at = RewriteTokens(cur + 1, close);
            cur = close + 1;
          } else {
            close_at = RewriteTokens(cur, e);
            cur = e;
          }
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      std::string q = "SELECT * FROM pf_intervals_from_events(" +
                      SqlQuote(rel) + ", per := " + SqlQuoteList(per) +
                      ", full_row := true";
      if (!stop_rel.empty())
        q += ", stop := " + SqlQuote(stop_rel);
      if (keep_open)
        q += ", keep_last_open := true";
      if (!close_at.empty())
        q += ", close_last_at := " + SqlQuote(close_at);
      q += ")";
      return q;
    }

    // 4. `GRAPH (DFS | BFS | DOMINATOR) TREE NODES <n> EDGES <ed> FROM <s> [PER
    // ...]`
    if (IsKw(b, "GRAPH") &&
        (IsKw(b + 1, "DFS") || IsKw(b + 1, "BFS") ||
         IsKw(b + 1, "DOMINATOR")) &&
        IsKw(b + 2, "TREE")) {
      std::string kind = LowerAscii(toks_[b + 1].text);
      size_t cur = b + 3;
      std::string nodes, edges, seeds;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "NODES")) {
          cur++;
          nodes = ParseRelation(&cur, e);
        } else if (IsKw(cur, "EDGES")) {
          cur++;
          edges = ParseRelation(&cur, e);
        } else if (IsKw(cur, "FROM")) {
          cur++;
          seeds = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return GenGraphTreeSql(kind, nodes, edges, seeds, per, /*full_row=*/true,
                             threads_);
    }

    // 5. Any standard SQL query (SELECT ..., VALUES ..., etc.) at the head of
    //    a pipeline
    return RewriteTokens(b, e);
  }

  // -------------------------------------------------------------------------
  // Pipeline Stage Compilation (§2, §4, §5.2-5.3, §6)
  // -------------------------------------------------------------------------
  std::string CompileStage(const std::string& cur_sql,
                           size_t b,
                           size_t e,
                           std::string* cur_alias,
                           Strs* fork_ctes) {
    if (b >= e)
      return cur_sql;

    // `FORK AS <name>` (§2)
    if (IsKw(b, "FORK") && IsKw(b + 1, "AS") && b + 2 < e) {
      std::string fork_name = SqlIdent(toks_[b + 2].text);
      std::string snap = "__pf_fork_" + std::to_string((*fork_counter_)++);
      fork_ctes->push_back(snap + " AS (" + cur_sql + ")");
      fork_ctes->push_back(fork_name + " AS (SELECT * FROM " + snap + ")");
      return "SELECT * FROM " + snap;
    }

    // `AS <alias>`
    if (IsKw(b, "AS") && b + 1 < e) {
      *cur_alias = toks_[b + 1].text;
      return "SELECT * FROM (" + cur_sql + ") AS " + SqlIdent(*cur_alias);
    }

    // `WHERE <expr>` (§4)
    if (IsKw(b, "WHERE")) {
      return "SELECT * FROM " + FromTarget(cur_sql, *cur_alias) + " WHERE " +
             RewriteTokens(b + 1, e);
    }

    // `SELECT [DISTINCT] <items>` (§4)
    if (IsKw(b, "SELECT")) {
      size_t cur = b + 1;
      std::string dist;
      if (IsKw(cur, "DISTINCT")) {
        dist = "DISTINCT ";
        cur++;
      }
      std::string items =
          RewriteTokens(cur, e, /*star_except_to_exclude=*/true);
      bool has_star = false;
      for (size_t i = cur; i < e; i++) {
        if (toks_[i].kind == TokKind::kStar) {
          has_star = true;
          break;
        }
      }
      if (!has_star)
        struct_aliases_.clear();
      std::string res =
          "SELECT " + dist + items + " FROM " + FromTarget(cur_sql, *cur_alias);
      cur_alias->clear();
      return res;
    }

    // `EXTEND <items>` (§4)
    if (IsKw(b, "EXTEND")) {
      std::string items =
          RewriteTokens(b + 1, e, /*star_except_to_exclude=*/true);
      return "SELECT *, " + items + " FROM " + FromTarget(cur_sql, *cur_alias);
    }

    // `SET col = expr, ...` (§4)
    if (IsKw(b, "SET")) {
      auto ranges = SplitCommaRanges(b + 1, e);
      Strs reps;
      for (const auto& [rb, re] : ranges) {
        size_t eq = rb;
        int depth = 0;
        while (eq < re) {
          if (toks_[eq].kind == TokKind::kLParen)
            depth++;
          else if (toks_[eq].kind == TokKind::kRParen && depth > 0)
            depth--;
          else if (toks_[eq].kind == TokKind::kEq && depth == 0)
            break;
          eq++;
        }
        if (eq < re) {
          std::string col = RewriteTokens(rb, eq);
          std::string expr = RewriteTokens(eq + 1, re);
          reps.push_back("(" + expr + ") AS " + col);
        }
      }
      return "SELECT * REPLACE (" + JoinStrs(reps, ", ") + ") FROM " +
             FromTarget(cur_sql, *cur_alias);
    }

    // `DROP col, ...` (§4)
    if (IsKw(b, "DROP")) {
      Strs cols = ParseColList(&++b, e);
      Strs qcols;
      for (const auto& c : cols)
        qcols.push_back(SqlIdent(c));
      return "SELECT * EXCLUDE (" + JoinStrs(qcols, ", ") + ") FROM " +
             FromTarget(cur_sql, *cur_alias);
    }

    // `RENAME old AS new, ...` (§4)
    if (IsKw(b, "RENAME")) {
      return "SELECT * RENAME (" + RewriteTokens(b + 1, e) + ") FROM " +
             FromTarget(cur_sql, *cur_alias);
    }

    // `DISTINCT` (§4)
    if (IsKw(b, "DISTINCT")) {
      return "SELECT DISTINCT * FROM " + FromTarget(cur_sql, *cur_alias);
    }

    // `ORDER BY ...` (§4)
    if (IsKw(b, "ORDER") && IsKw(b + 1, "BY")) {
      return "SELECT * FROM " + FromTarget(cur_sql, *cur_alias) + " ORDER BY " +
             RewriteTokens(b + 2, e);
    }

    // `LIMIT ...` (§4)
    if (IsKw(b, "LIMIT")) {
      return "SELECT * FROM " + FromTarget(cur_sql, *cur_alias) + " LIMIT " +
             RewriteTokens(b + 1, e);
    }

    // `WINDOW expr AS name, ...` (§4)
    if (IsKw(b, "WINDOW")) {
      return "SELECT *, " + RewriteTokens(b + 1, e) + " FROM " +
             FromTarget(cur_sql, *cur_alias);
    }

    // `SQL ( ... @input ... )` (§4)
    if (IsKw(b, "SQL") && b + 1 < e && toks_[b + 1].kind == TokKind::kLParen) {
      size_t close = FindMatchingRParen(b + 1, e);
      if (close < e) {
        std::string res =
            RewriteTokens(b + 2, close, false, "(" + cur_sql + ")");
        cur_alias->clear();
        return res;
      }
    }

    // `AGGREGATE <aggs> [GROUP BY <keys_or_rollup>]` (§4)
    if (IsKw(b, "AGGREGATE")) {
      size_t grp_kw = e;
      int depth = 0;
      for (size_t i = b + 1; i < e; i++) {
        if (toks_[i].kind == TokKind::kLParen) {
          depth++;
        } else if (toks_[i].kind == TokKind::kRParen && depth > 0) {
          depth--;
        } else if (depth == 0 && IsKw(i, "GROUP") && IsKw(i + 1, "BY")) {
          grp_kw = i;
          break;
        }
      }
      std::string aggs_str = RewriteTokens(b + 1, grp_kw);
      if (grp_kw == e) {
        struct_aliases_.clear();
        std::string res =
            "SELECT " + aggs_str + " FROM " + FromTarget(cur_sql, *cur_alias);
        cur_alias->clear();
        return res;
      }
      size_t gstart = grp_kw + 2;
      // Check for GROUP BY ROLLUP(...)
      if (IsKw(gstart, "ROLLUP") && gstart + 1 < e &&
          toks_[gstart + 1].kind == TokKind::kLParen) {
        size_t close = FindMatchingRParen(gstart + 1, e);
        std::string rollup_cols = RewriteTokens(gstart + 2, close);
        struct_aliases_.clear();
        std::string select_list = rollup_cols;
        if (!aggs_str.empty())
          select_list += ", " + aggs_str;
        std::string res = "SELECT " + select_list + " FROM " +
                          FromTarget(cur_sql, *cur_alias) +
                          " GROUP BY ROLLUP(" + rollup_cols + ")";
        cur_alias->clear();
        return res;
      }
      // Standard GROUP BY key1 [AS alias1], key2 [AS alias2], ...
      auto key_ranges = SplitCommaRanges(gstart, e);
      Strs select_keys, group_exprs;
      for (const auto& [kb, ke] : key_ranges) {
        select_keys.push_back(RewriteTokens(kb, ke));
        // Strip top-level trailing `AS <alias>` for the GROUP BY clause
        size_t as_pos = ke;
        int d = 0;
        for (size_t i = kb; i < ke; i++) {
          if (toks_[i].kind == TokKind::kLParen)
            d++;
          else if (toks_[i].kind == TokKind::kRParen && d > 0)
            d--;
          else if (d == 0 && IsKw(i, "AS"))
            as_pos = i;
        }
        group_exprs.push_back(RewriteTokens(kb, as_pos));
      }
      struct_aliases_.clear();
      std::string select_list = JoinStrs(select_keys, ", ");
      if (!aggs_str.empty())
        select_list += ", " + aggs_str;
      std::string res = "SELECT " + select_list + " FROM " +
                        FromTarget(cur_sql, *cur_alias) + " GROUP BY " +
                        JoinStrs(group_exprs, ", ");
      cur_alias->clear();
      return res;
    }

    // `(UNION | INTERSECT | EXCEPT) [ALL] [BY NAME] (<pipeline_or_select>)`
    // (§4)
    if (IsKw(b, "UNION") || IsKw(b, "INTERSECT") || IsKw(b, "EXCEPT")) {
      std::string op = toks_[b].upper;
      size_t cur = b + 1;
      if (IsKw(cur, "ALL")) {
        op += " ALL";
        cur++;
      } else if (IsKw(cur, "DISTINCT")) {
        op += " DISTINCT";
        cur++;
      }
      if (IsKw(cur, "BY") && IsKw(cur + 1, "NAME")) {
        op += " BY NAME";
        cur += 2;
      }
      std::string rhs;
      if (cur < e && toks_[cur].kind == TokKind::kLParen) {
        size_t close = FindMatchingRParen(cur, e);
        rhs = CompileQueryExpr(cur + 1, close);
      } else {
        rhs = CompileQueryExpr(cur, e);
      }
      cur_alias->clear();
      return "SELECT * FROM (" + cur_sql + ") " + op + " SELECT * FROM (" +
             rhs + ")";
    }

    // `[LEFT] INTERVAL ...` (§5.2 - §5.3)
    if (IsKw(b, "INTERVAL") || (IsKw(b, "LEFT") && IsKw(b + 1, "INTERVAL"))) {
      bool is_left = IsKw(b, "LEFT");
      size_t cur = is_left ? b + 2 : b + 1;
      return CompileIntervalStage(cur_sql, cur, e, is_left, cur_alias);
    }

    // `TREE ...` (§6)
    if (IsKw(b, "TREE")) {
      return CompileTreeStage(cur_sql, b + 1, e, cur_alias);
    }

    // `[NATURAL] [INNER|LEFT|RIGHT|FULL|CROSS|SEMI|ANTI] [OUTER] JOIN <rel>
    //  [[AS] <alias>] [ON <expr> | USING (<cols>)]` (§4)
    {
      size_t cur = b;
      std::string join_prefix;
      while (cur < e &&
             (IsKw(cur, "NATURAL") || IsKw(cur, "INNER") || IsKw(cur, "LEFT") ||
              IsKw(cur, "RIGHT") || IsKw(cur, "FULL") || IsKw(cur, "CROSS") ||
              IsKw(cur, "SEMI") || IsKw(cur, "ANTI") || IsKw(cur, "OUTER"))) {
        join_prefix += toks_[cur].upper + " ";
        cur++;
      }
      if (IsKw(cur, "JOIN")) {
        cur++;
        std::string rel = ParseRelation(&cur, e);
        std::string r_alias;
        if (IsKw(cur, "AS") && cur + 1 < e) {
          r_alias = " AS " + SqlIdent(toks_[cur + 1].text);
          cur += 2;
        } else if (cur < e && toks_[cur].kind == TokKind::kWord &&
                   !IsKw(cur, "ON") && !IsKw(cur, "USING")) {
          r_alias = " AS " + SqlIdent(toks_[cur].text);
          cur++;
        }
        std::string cond = (cur < e) ? " " + RewriteTokens(cur, e) : "";
        return "SELECT * FROM " + FromTarget(cur_sql, *cur_alias) + " " +
               join_prefix + "JOIN " + rel + r_alias + cond;
      }
    }

    return SqlError("pf_pipe: unrecognized pipeline stage: " +
                    RewriteTokens(b, e));
  }

  // -------------------------------------------------------------------------
  // Interval Stage Compilation (§5.2 - §5.3)
  // -------------------------------------------------------------------------
  // Parses a relationship phrase:
  //   OVERLAPPING [BOUNDS]
  //   COVERING (BEGIN | END | BOUNDS)
  //   WITHIN [BOUNDS]
  std::string ParseRelationship(size_t* cur, size_t /*e*/) {
    if (IsKw(*cur, "OVERLAPPING")) {
      (*cur)++;
      if (IsKw(*cur, "BOUNDS"))
        (*cur)++;
      return "overlapping bounds";
    }
    if (IsKw(*cur, "COVERING")) {
      (*cur)++;
      if (IsKw(*cur, "BEGIN") || IsKw(*cur, "END") || IsKw(*cur, "BOUNDS")) {
        std::string sub = LowerAscii(toks_[*cur].text);
        (*cur)++;
        return "covering " + sub;
      }
      return "covering bounds";
    }
    if (IsKw(*cur, "WITHIN")) {
      (*cur)++;
      if (IsKw(*cur, "BOUNDS"))
        (*cur)++;
      return "within bounds";
    }
    return "overlapping bounds";
  }

  // Helper for `PER ROLLUP(c1, c2, ...)` on collapsing interval stages
  // (`INTERVAL FLATTEN`, `INTERVAL MERGE OVERLAPPING`,
  // `INTERVAL MERGE CONSECUTIVE`).
  template <typename Fn>
  std::string ExpandPerRollup(const Strs& per, Fn&& make_level_sql) {
    Strs levels;
    for (size_t k = per.size() + 1; k-- > 0;) {
      Strs prefix(per.begin(), per.begin() + k);
      std::string sub = make_level_sql(prefix);
      Strs proj;
      for (size_t j = k; j < per.size(); j++)
        proj.push_back("NULL AS " + SqlIdent(per[j]));
      for (size_t j = 0; j < per.size(); j++) {
        proj.push_back(std::string(j < k ? "0" : "1") + " AS grouping_" +
                       per[j]);
      }
      levels.push_back("SELECT *Field_Placeholder* FROM (" + sub + ")");
      std::string extra = JoinStrs(proj, ", ");
      levels.back() = "SELECT *" + (extra.empty() ? "" : ", " + extra) +
                      " FROM (" + sub + ")";
    }
    return JoinStrs(levels, " UNION ALL BY NAME ");
  }

  std::string CompileIntervalStage(const std::string& cur_sql,
                                   size_t cur,
                                   size_t e,
                                   bool is_left,
                                   std::string* cur_alias) {
    std::string in_rel = "(" + cur_sql + ")";

    // `INTERVAL SPLIT <rel> AS <alias> [PER <per>]`
    if (IsKw(cur, "SPLIT")) {
      cur++;
      std::string imp;
      std::string op_rel = ParseRelation(&cur, e, &imp);
      std::string al = imp.empty() ? "right_row" : imp;
      if (IsKw(cur, "AS") && cur + 1 < e) {
        al = toks_[cur + 1].text;
        cur += 2;
      }
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      std::string sql_al = RegisterStructAlias(al, per);
      return "SELECT * FROM pf_interval_split(" + SqlQuote(in_rel) + ", " +
             SqlQuote(op_rel) + ", " + SqlQuote(sql_al) +
             ", per := " + SqlQuoteList(per) + ")";
    }

    // `INTERVAL SUBTRACT <rel> [PER <per>]`
    if (IsKw(cur, "SUBTRACT")) {
      cur++;
      std::string op_rel = ParseRelation(&cur, e);
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_interval_subtract(" + SqlQuote(in_rel) + ", " +
             SqlQuote(op_rel) + ", per := " + SqlQuoteList(per) + ")";
    }

    // `INTERVAL FILL GAPS WITH <rel> [PER <per>]`
    if (IsKw(cur, "FILL") &&
        ((IsKw(cur + 1, "GAPS") && IsKw(cur + 2, "WITH")) ||
         IsKw(cur + 1, "WITHIN"))) {
      cur += IsKw(cur + 1, "GAPS") ? 3 : 2;
      std::string bg_rel = ParseRelation(&cur, e);
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_interval_fill_gaps(" + SqlQuote(in_rel) + ", " +
             SqlQuote(bg_rel) + ", per := " + SqlQuoteList(per) +
             ", bg_has_per := NULL)";
    }

    // `INTERVAL FLATTEN [PER [ROLLUP (] <per> [)]] [AGGREGATE <aggs>]`
    if (IsKw(cur, "FLATTEN")) {
      cur_alias->clear();
      cur++;
      Strs per;
      bool rollup = false;
      Strs aggs;
      while (cur < e) {
        if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e, &rollup);
        } else if (IsKw(cur, "AGGREGATE")) {
          cur++;
          aggs = SplitCommaItems(cur, e);
          cur = e;
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      struct_aliases_.clear();
      if (!rollup)
        return GenIntervalFlattenSql(in_rel, aggs, per, threads_);
      return ExpandPerRollup(per, [&](const Strs& pfx) {
        return GenIntervalFlattenSql(in_rel, aggs, pfx, threads_);
      });
    }

    // `INTERVAL MERGE OVERLAPPING [PER [ROLLUP (] <per> [)]] [AGGREGATE
    // <aggs>]` `INTERVAL MERGE CONSECUTIVE BY <cols> [PER [ROLLUP (] <per> [)]]
    // [AGGREGATE <aggs>]`
    if (IsKw(cur, "MERGE")) {
      cur_alias->clear();
      cur++;
      if (IsKw(cur, "OVERLAPPING")) {
        cur++;
        Strs per;
        bool rollup = false;
        Strs aggs;
        while (cur < e) {
          if (IsKw(cur, "PER")) {
            cur++;
            per = ParsePerClause(&cur, e, &rollup);
          } else if (IsKw(cur, "AGGREGATE")) {
            cur++;
            aggs = SplitCommaItems(cur, e);
            cur = e;
          } else {
            cur++;
          }
        }
        per = EffectivePer(per);
        struct_aliases_.clear();
        auto make_one = [&](const Strs& pfx) -> std::string {
          if (aggs.empty()) {
            return "SELECT * EXCLUDE (__pf_cnt) FROM "
                   "pf_interval_merge_overlapping(" +
                   SqlQuote(in_rel) +
                   ", 'count(*) AS __pf_cnt', per := " + SqlQuoteList(pfx) +
                   ")";
          }
          return "SELECT * FROM pf_interval_merge_overlapping(" +
                 SqlQuote(in_rel) + ", " + SqlQuoteList(aggs) +
                 ", per := " + SqlQuoteList(pfx) + ")";
        };
        if (!rollup)
          return make_one(per);
        return ExpandPerRollup(per, make_one);
      }
      if (IsKw(cur, "CONSECUTIVE")) {
        cur++;
        Strs by_cols;
        Strs per;
        bool rollup = false;
        Strs aggs;
        while (cur < e) {
          if (IsKw(cur, "BY")) {
            cur++;
            by_cols = ParseColList(&cur, e);
          } else if (IsKw(cur, "PER")) {
            cur++;
            per = ParsePerClause(&cur, e, &rollup);
          } else if (IsKw(cur, "AGGREGATE")) {
            cur++;
            aggs = SplitCommaItems(cur, e);
            cur = e;
          } else {
            cur++;
          }
        }
        per = EffectivePer(per);
        struct_aliases_.clear();
        auto make_one = [&](const Strs& pfx) -> std::string {
          if (aggs.empty()) {
            return "SELECT * EXCLUDE (__pf_cnt) FROM "
                   "pf_interval_merge_consecutive(" +
                   SqlQuote(in_rel) + ", " + SqlQuoteList(by_cols) +
                   ", 'count(*) AS __pf_cnt', per := " + SqlQuoteList(pfx) +
                   ")";
          }
          return "SELECT * FROM pf_interval_merge_consecutive(" +
                 SqlQuote(in_rel) + ", " + SqlQuoteList(by_cols) + ", " +
                 SqlQuoteList(aggs) + ", per := " + SqlQuoteList(pfx) + ")";
        };
        if (!rollup)
          return make_one(per);
        return ExpandPerRollup(per, make_one);
      }
    }

    // `INTERVAL SET BOUNDS BEGIN AT <expr> (END AT <expr> | FOR <expr>) [PER
    // <per>]`
    if (IsKw(cur, "SET") && IsKw(cur + 1, "BOUNDS")) {
      cur += 2;
      if (IsKw(cur, "BEGIN") && IsKw(cur + 1, "AT"))
        cur += 2;
      size_t b_start = cur;
      int depth = 0;
      while (cur < e) {
        if (toks_[cur].kind == TokKind::kLParen)
          depth++;
        else if (toks_[cur].kind == TokKind::kRParen && depth > 0)
          depth--;
        else if (depth == 0 && ((IsKw(cur, "END") && IsKw(cur + 1, "AT")) ||
                                IsKw(cur, "FOR"))) {
          break;
        }
        cur++;
      }
      std::string begin_at = RewriteTokens(b_start, cur);
      bool is_end_at = false;
      if (IsKw(cur, "END") && IsKw(cur + 1, "AT")) {
        is_end_at = true;
        cur += 2;
      } else if (IsKw(cur, "FOR")) {
        cur++;
      }
      size_t e_start = cur;
      depth = 0;
      while (cur < e) {
        if (toks_[cur].kind == TokKind::kLParen)
          depth++;
        else if (toks_[cur].kind == TokKind::kRParen && depth > 0)
          depth--;
        else if (depth == 0 && IsKw(cur, "PER"))
          break;
        cur++;
      }
      std::string second_expr = RewriteTokens(e_start, cur);
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_interval_set_bounds(" + SqlQuote(in_rel) + ", " +
             SqlQuote(begin_at) + ", " +
             (is_end_at ? "end_at := " : "for_dur := ") +
             SqlQuote(second_expr) + ", per := " + SqlQuoteList(per) + ")";
    }

    // `INTERVAL QUANTIZE <width> [ALIGNED TO (TRACE START | ZERO | <expr>)]
    // [PER <per>]`
    if (IsKw(cur, "QUANTIZE")) {
      cur++;
      size_t w_start = cur;
      int depth = 0;
      while (cur < e) {
        if (toks_[cur].kind == TokKind::kLParen)
          depth++;
        else if (toks_[cur].kind == TokKind::kRParen && depth > 0)
          depth--;
        else if (depth == 0 && (IsKw(cur, "ALIGNED") || IsKw(cur, "PER")))
          break;
        cur++;
      }
      std::string width = RewriteTokens(w_start, cur);
      std::string aligned = "trace_start";
      if (IsKw(cur, "ALIGNED") && IsKw(cur + 1, "TO")) {
        cur += 2;
        if (IsKw(cur, "TRACE") && IsKw(cur + 1, "START")) {
          aligned = "trace_start";
          cur += 2;
        } else if (IsKw(cur, "ZERO")) {
          aligned = "zero";
          cur++;
        } else {
          size_t a_start = cur;
          depth = 0;
          while (cur < e) {
            if (toks_[cur].kind == TokKind::kLParen)
              depth++;
            else if (toks_[cur].kind == TokKind::kRParen && depth > 0)
              depth--;
            else if (depth == 0 && IsKw(cur, "PER"))
              break;
            cur++;
          }
          aligned = RewriteTokens(a_start, cur);
        }
      }
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_interval_quantize(" + SqlQuote(in_rel) + ", " +
             width + ", aligned_to := " + SqlQuote(aligned) +
             ", per := " + SqlQuoteList(per) + ")";
    }

    // `[LEFT] INTERVAL JOIN <rel> AS <alias> [<relationship>] [PER <per>]`
    if (IsKw(cur, "JOIN")) {
      cur++;
      std::string imp;
      std::string op_rel = ParseRelation(&cur, e, &imp);
      std::string al = imp.empty() ? "right_row" : imp;
      if (IsKw(cur, "AS") && cur + 1 < e) {
        al = toks_[cur + 1].text;
        cur += 2;
      }
      std::string rel_kind = "overlapping bounds";
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else if (IsKw(cur, "OVERLAPPING") || IsKw(cur, "COVERING") ||
                   IsKw(cur, "WITHIN")) {
          rel_kind = ParseRelationship(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      std::string sql_al = RegisterStructAlias(al, per);
      return "SELECT * FROM pf_interval_join(" + SqlQuote(in_rel) + ", " +
             SqlQuote(op_rel) + ", " + SqlQuote(sql_al) + ", " +
             SqlQuote(rel_kind) + ", per := " + SqlQuoteList(per) +
             ", left_join := " + (is_left ? "true" : "false") + ")";
    }

    // `[LEFT] INTERVAL FIND <rel> AS <alias> (STARTING | ENDING) (AFTER |
    // BEFORE)
    //  (BEGIN | END | BOUNDS) [OF <anchor_alias>] [WITHIN <dur>] [PER <per>]`
    if (IsKw(cur, "FIND")) {
      cur++;
      std::string imp;
      std::string op_rel = ParseRelation(&cur, e, &imp);
      std::string al = imp.empty() ? "right_row" : imp;
      if (IsKw(cur, "AS") && cur + 1 < e) {
        al = toks_[cur + 1].text;
        cur += 2;
      }
      std::string edge = "starting";
      if (IsKw(cur, "STARTING") || IsKw(cur, "ENDING")) {
        edge = LowerAscii(toks_[cur].text);
        cur++;
      }
      std::string dir = "after";
      if (IsKw(cur, "AFTER") || IsKw(cur, "BEFORE")) {
        dir = LowerAscii(toks_[cur].text);
        cur++;
      }
      std::string anchor = "begin";
      if (IsKw(cur, "BEGIN") || IsKw(cur, "END") || IsKw(cur, "BOUNDS")) {
        anchor = LowerAscii(toks_[cur].text);
        cur++;
        if (IsKw(cur, "OF") && cur + 1 < e) {
          std::string anc_al = toks_[cur + 1].text;
          auto it = struct_aliases_.find(LowerAscii(anc_al));
          if (it != struct_aliases_.end())
            anc_al = it->second;
          anchor += " of " + anc_al;
          cur += 2;
        }
      }
      std::string within;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "WITHIN")) {
          cur++;
          size_t w_start = cur;
          while (cur < e && !IsKw(cur, "PER"))
            cur++;
          within = RewriteTokens(w_start, cur);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      std::string sql_al = RegisterStructAlias(al, per);
      std::string q = "SELECT * FROM pf_interval_find(" + SqlQuote(in_rel) +
                      ", " + SqlQuote(op_rel) + ", " + SqlQuote(sql_al) + ", " +
                      SqlQuote(edge) + ", " + SqlQuote(dir) + ", " +
                      SqlQuote(anchor) + ", per := " + SqlQuoteList(per) +
                      ", left_join := " + (is_left ? "true" : "false");
      if (!within.empty())
        q += ", within := " + SqlQuote(within);
      q += ")";
      return q;
    }

    // `INTERVAL (KEEP | DROP) IF [<relationship>] <rel> [PER <per>]`
    if ((IsKw(cur, "KEEP") || IsKw(cur, "DROP")) && IsKw(cur + 1, "IF")) {
      bool is_keep = IsKw(cur, "KEEP");
      cur += 2;
      std::string rel_kind = "overlapping bounds";
      if (IsKw(cur, "OVERLAPPING") || IsKw(cur, "COVERING") ||
          IsKw(cur, "WITHIN")) {
        rel_kind = ParseRelationship(&cur, e);
      }
      std::string op_rel = ParseRelation(&cur, e);
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      std::string fn = is_keep ? "pf_interval_keep_if" : "pf_interval_drop_if";
      return "SELECT * FROM " + fn + "(" + SqlQuote(in_rel) + ", " +
             SqlQuote(op_rel) + ", " + SqlQuote(rel_kind) +
             ", per := " + SqlQuoteList(per) + ")";
    }

    return SqlError("pf_pipe: unrecognized INTERVAL stage: " +
                    RewriteTokens(cur, e));
  }

  // -------------------------------------------------------------------------
  // Tree Stage Compilation (§6)
  // -------------------------------------------------------------------------
  std::string CompileTreeStage(const std::string& cur_sql,
                               size_t cur,
                               size_t e,
                               std::string* /*cur_alias*/) {
    std::string in_rel = "(" + cur_sql + ")";

    // `TREE (KEEP | DROP) IF (ANCESTOR | DESCENDANT) OF <rel> [OVER <over>]
    // [PER <per>]`
    if ((IsKw(cur, "KEEP") || IsKw(cur, "DROP")) && IsKw(cur + 1, "IF")) {
      std::string op = IsKw(cur, "KEEP") ? "keep_" : "drop_";
      cur += 2;
      std::string rel_dir = "descendant";
      if (IsKw(cur, "ANCESTOR") || IsKw(cur, "DESCENDANT")) {
        rel_dir = LowerAscii(toks_[cur].text);
        cur++;
      }
      if (IsKw(cur, "OF"))
        cur++;
      std::string marker = ParseRelation(&cur, e);
      std::string over;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "OVER")) {
          cur++;
          over = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return GenTreeFilterSql(op + rel_dir, in_rel, marker, per, over,
                              /*full_row=*/true, threads_);
    }

    // `TREE PRUNE AT <rel> [OVER <over>] [PER <per>]`
    if (IsKw(cur, "PRUNE")) {
      cur++;
      if (IsKw(cur, "AT"))
        cur++;
      std::string marker = ParseRelation(&cur, e);
      std::string over;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "OVER")) {
          cur++;
          over = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return GenTreeFilterSql("prune", in_rel, marker, per, over,
                              /*full_row=*/true, threads_);
    }

    // `TREE (CONTRACT | ABSORB) AT <rel> [OVER <over>] [PER <per>] AGGREGATE
    // <aggs>`
    if (IsKw(cur, "CONTRACT") || IsKw(cur, "ABSORB")) {
      bool is_contract = IsKw(cur, "CONTRACT");
      cur++;
      if (IsKw(cur, "AT"))
        cur++;
      std::string marker = ParseRelation(&cur, e);
      std::string over;
      Strs per;
      Strs aggs;
      while (cur < e) {
        if (IsKw(cur, "OVER")) {
          cur++;
          over = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else if (IsKw(cur, "AGGREGATE")) {
          cur++;
          aggs = SplitCommaItems(cur, e);
          cur = e;
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      std::string fn = is_contract ? "pf_tree_contract" : "pf_tree_absorb";
      return "SELECT * FROM " + fn + "(" + SqlQuote(in_rel) + ", " +
             SqlQuote(marker) + ", " + SqlQuoteList(aggs) +
             ", per := " + SqlQuoteList(per) +
             (over.empty() ? "" : ", over := " + SqlQuote(over)) +
             ", full_row := true)";
    }

    // `TREE REPARENT [TO NEAREST ANCESTOR IN] <rel> [OVER <over>] [PER <per>]`
    if (IsKw(cur, "REPARENT")) {
      cur++;
      while (cur < e && (IsKw(cur, "TO") || IsKw(cur, "NEAREST") ||
                         IsKw(cur, "ANCESTOR") || IsKw(cur, "IN"))) {
        cur++;
      }
      std::string marker = ParseRelation(&cur, e);
      std::string over;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "OVER")) {
          cur++;
          over = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_reparent(" + SqlQuote(in_rel) + ", " +
             SqlQuote(marker) + ", per := " + SqlQuoteList(per) +
             (over.empty() ? "" : ", over := " + SqlQuote(over)) + ")";
    }

    // `TREE FLATTEN RECURSION BY <cols> [OVER <over>] [PER <per>]`
    if (IsKw(cur, "FLATTEN") && IsKw(cur + 1, "RECURSION")) {
      cur += 2;
      Strs keys;
      std::string over;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "BY")) {
          cur++;
          keys = ParseColList(&cur, e);
        } else if (IsKw(cur, "OVER")) {
          cur++;
          over = ParseRelation(&cur, e);
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_flatten_recursion(" + SqlQuote(in_rel) +
             ", " + SqlQuoteList(keys) + ", per := " + SqlQuoteList(per) +
             (over.empty() ? "" : ", over := " + SqlQuote(over)) + ")";
    }

    // `TREE KEY SIBLINGS BY <cols> AS (<key_col>, <parent_key_col>) [ORDERED]
    // [PER <per>]`
    if (IsKw(cur, "KEY") && IsKw(cur + 1, "SIBLINGS")) {
      cur += 2;
      Strs keys;
      std::string key_col = "node_id";
      std::string pkey_col = "parent_node_id";
      bool ordered = false;
      Strs per;
      while (cur < e) {
        if (IsKw(cur, "BY")) {
          cur++;
          keys = ParseColList(&cur, e);
        } else if (IsKw(cur, "AS") && cur + 1 < e &&
                   toks_[cur + 1].kind == TokKind::kLParen) {
          size_t close = FindMatchingRParen(cur + 1, e);
          cur += 2;
          Strs kc = ParseColList(&cur, close);
          if (kc.size() >= 2) {
            key_col = kc[0];
            pkey_col = kc[1];
          }
          cur = close + 1;
        } else if (IsKw(cur, "ORDERED")) {
          ordered = true;
          cur++;
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_key_siblings(" + SqlQuote(in_rel) + ", " +
             SqlQuoteList(keys) + ", " + SqlQuote(key_col) + ", " +
             SqlQuote(pkey_col) + ", per := " + SqlQuoteList(per) +
             ", ordered := " + (ordered ? "true" : "false") + ")";
    }

    // `TREE MERGE SIBLINGS BY <cols> [ORDERED] [PER <per>] AGGREGATE <aggs>`
    if (IsKw(cur, "MERGE") && IsKw(cur + 1, "SIBLINGS")) {
      cur += 2;
      Strs keys;
      bool ordered = false;
      Strs per;
      Strs aggs;
      while (cur < e) {
        if (IsKw(cur, "BY")) {
          cur++;
          keys = ParseColList(&cur, e);
        } else if (IsKw(cur, "ORDERED")) {
          ordered = true;
          cur++;
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else if (IsKw(cur, "AGGREGATE")) {
          cur++;
          aggs = SplitCommaItems(cur, e);
          cur = e;
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_merge_siblings(" + SqlQuote(in_rel) + ", " +
             SqlQuoteList(keys) + ", " + SqlQuoteList(aggs) +
             ", per := " + SqlQuoteList(per) +
             ", ordered := " + (ordered ? "true" : "false") + ")";
    }

    // `TREE MERGE INTO PARENT BY <cols> [INDIRECT] [PER <per>] AGGREGATE
    // <aggs>`
    if (IsKw(cur, "MERGE") && IsKw(cur + 1, "INTO") &&
        IsKw(cur + 2, "PARENT")) {
      cur += 3;
      Strs keys;
      bool indirect = false;
      Strs per;
      Strs aggs;
      while (cur < e) {
        if (IsKw(cur, "BY")) {
          cur++;
          keys = ParseColList(&cur, e);
        } else if (IsKw(cur, "INDIRECT")) {
          indirect = true;
          cur++;
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else if (IsKw(cur, "AGGREGATE")) {
          cur++;
          aggs = SplitCommaItems(cur, e);
          cur = e;
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_merge_into_parent(" + SqlQuote(in_rel) +
             ", " + SqlQuoteList(keys) + ", " + SqlQuoteList(aggs) +
             ", per := " + SqlQuoteList(per) +
             ", indirect := " + (indirect ? "true" : "false") + ")";
    }

    // `TREE INVERT [PER <per>]`
    if (IsKw(cur, "INVERT")) {
      cur++;
      Strs per;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
      }
      per = EffectivePer(per);
      return "SELECT * FROM pf_tree_invert(" + SqlQuote(in_rel) +
             ", per := " + SqlQuoteList(per) + ")";
    }

    // `TREE ACCUMULATE (UP | DOWN) [AGGREGATE] <aggs> [PER <per>]`
    if (IsKw(cur, "ACCUMULATE")) {
      cur++;
      std::string dir = "up";
      if (IsKw(cur, "UP") || IsKw(cur, "DOWN")) {
        dir = LowerAscii(toks_[cur].text);
        cur++;
      }
      if (IsKw(cur, "AGGREGATE"))
        cur++;
      // Find optional trailing/leading PER clause
      Strs per;
      size_t agg_start = cur;
      size_t agg_end = e;
      if (IsKw(cur, "PER")) {
        cur++;
        per = ParsePerClause(&cur, e);
        if (IsKw(cur, "AGGREGATE"))
          cur++;
        agg_start = cur;
      } else {
        int depth = 0;
        for (size_t i = cur; i < e; i++) {
          if (toks_[i].kind == TokKind::kLParen)
            depth++;
          else if (toks_[i].kind == TokKind::kRParen && depth > 0)
            depth--;
          else if (depth == 0 && IsKw(i, "PER")) {
            agg_end = i;
            size_t pc = i + 1;
            per = ParsePerClause(&pc, e);
            break;
          }
        }
      }
      Strs aggs = SplitCommaItems(agg_start, agg_end);
      per = EffectivePer(per);
      return GenTreeAccumulateSql(in_rel, dir, aggs, per, /*full_row=*/true,
                                  threads_);
    }

    // `TREE EXPAND (UP | DOWN) <rel> BY <cols> [ORDERED BY <cols>]
    //  [CHARGE <cols> TO (LEAF | ROOT)] [PER <per>]`
    if (IsKw(cur, "EXPAND")) {
      cur++;
      std::string dir = "down";
      if (IsKw(cur, "UP") || IsKw(cur, "DOWN")) {
        dir = LowerAscii(toks_[cur].text);
        cur++;
      }
      std::string op_rel = ParseRelation(&cur, e);
      Strs by_cols, ord_cols, charge_cols, per;
      std::string charge_to;
      while (cur < e) {
        if (IsKw(cur, "BY")) {
          cur++;
          by_cols = ParseColList(&cur, e);
        } else if (IsKw(cur, "ORDERED") && IsKw(cur + 1, "BY")) {
          cur += 2;
          ord_cols = ParseColList(&cur, e);
        } else if (IsKw(cur, "CHARGE")) {
          cur++;
          charge_cols = ParseColList(&cur, e);
          if (IsKw(cur, "TO") && cur + 1 < e) {
            charge_to = LowerAscii(toks_[cur + 1].text);
            cur += 2;
          }
        } else if (IsKw(cur, "PER")) {
          cur++;
          per = ParsePerClause(&cur, e);
        } else {
          cur++;
        }
      }
      per = EffectivePer(per);
      std::string q = "SELECT * FROM pf_tree_expand(" + SqlQuote(in_rel) +
                      ", " + SqlQuote(dir) + ", " + SqlQuote(op_rel) + ", " +
                      SqlQuoteList(by_cols) +
                      ", ordered_by := " + SqlQuoteList(ord_cols) +
                      ", charge := " + SqlQuoteList(charge_cols);
      if (!charge_to.empty())
        q += ", charge_to := " + SqlQuote(charge_to);
      q += ", per := " + SqlQuoteList(per) + ")";
      return q;
    }

    return SqlError("pf_pipe: unrecognized TREE stage: " +
                    RewriteTokens(cur, e));
  }

  const std::string& src_;
  std::vector<Tok> toks_;
  const Strs& default_per_;
  int64_t threads_;
  int* fork_counter_;
  std::unordered_map<std::string, std::string> struct_aliases_;
};

}  // namespace

std::string CompilePipeSql(const std::string& sql,
                           const std::vector<std::string>& default_per,
                           int64_t threads) {
  std::vector<Tok> toks = Tokenize(sql);
  if (toks.size() <= 1)
    return sql;
  size_t n = toks.size() - 1;
  int fork_counter = 0;
  PipeCompiler compiler(sql, std::move(toks), default_per, threads,
                        &fork_counter);
  return compiler.CompileScript(0, n);
}
