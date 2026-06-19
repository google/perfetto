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

#include "src/trace_processor/duckdb/duckdb_engine.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"
#include "src/trace_processor/duckdb/scalar_functions.h"
#include "src/trace_processor/duckdb/table_provider.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// The tiny static allowlist of scalar/aggregate function names DuckDB is known
// to evaluate with semantics matching SQLite for the beachhead query set. Any
// function NOT in this set makes the query ineligible (default-deny): it could
// be a runtime PerfettoSQL function, a custom intrinsic, or a builtin whose
// semantics diverge. Grown by later waves as functions are validated.
const std::unordered_set<std::string>& BuiltinFunctionAllowlist() {
  static const std::unordered_set<std::string>* kAllow =
      new std::unordered_set<std::string>{
          // Aggregates (beachhead).
          "count", "sum", "min", "max", "avg",
          // Scalar math builtins that are 1:1 DuckDB-native AND reach DuckDB
          // under their own name (they are NOT rewritten by PerfettoSQL's
          // `DELEGATES TO __intrinsic_*`, so the surface name is what binds),
          // AND whose result TYPE matches SQLite for the tested inputs. The
          // delegated math surface (`sqrt`/`ln`/`exp`/`regexp_extract`/`unhex`)
          // is handled by registered scalar UDFs (RegisterScalarFunctions), not
          // here.
          //
          // `ceil`/`floor` are deliberately EXCLUDED: DuckDB's `ceil(INTEGER)`
          // returns a DOUBLE (e.g. `ceil(5)` -> 5.000000) whereas SQLite returns
          // an INTEGER (`5`), a type divergence that fails the byte-exact diff.
          // (`trunc(INTEGER)` happens to stay INTEGER in DuckDB, so trunc is OK.)
          "abs", "round", "trunc", "pow", "power",
      };
  return *kAllow;
}

// SQL keywords that may legally appear in identifier position but are NOT
// relations/functions. A token matching one of these is ignored by the
// extractor (it is structural, not a name to resolve).
bool IsSqlKeyword(const std::string& lower) {
  static const std::unordered_set<std::string>* kKeywords =
      new std::unordered_set<std::string>{
          "select", "from",   "where",  "group",  "by",     "order",
          "having", "limit",  "offset", "as",     "and",    "or",
          "not",    "null",   "is",     "in",     "like",   "glob",
          "between","asc",    "desc",   "distinct","all",   "on",
          "join",   "inner",  "outer",  "left",   "right",  "full",
          "cross",  "using",  "union",  "intersect","except","case",
          "when",   "then",   "else",   "end",    "cast",   "true",
          "false",  "exists", "with",   "values", "default",
      };
  return kKeywords->find(lower) != kKeywords->end();
}

// A single extracted token of interest.
struct Token {
  enum Kind { kIdentifier, kSemicolon, kOther, kDoubleQuoted };
  Kind kind;
  std::string text;        // lowercased, for identifiers.
  bool followed_by_paren;  // identifier immediately followed by '(' => fn call.
  bool after_from_or_join; // identifier in relation position.
  // True for an identifier that is the NAME of a CTE with a column list, i.e.
  // the `data` in `WITH data(unit, time) AS (...)`. Such an identifier is
  // followed by '(' but is NOT a function call (the parenthesized list is the
  // CTE's column names), so it must NOT be checked against the function
  // allowlist.
  bool is_cte_column_list = false;
  // True for an identifier that DEFINES a CTE (the `data` in `WITH data AS ...`
  // or `WITH data(...) AS ...`). A later `FROM data` references this local CTE,
  // not an external relation, so it must NOT be checked by the relation
  // resolver.
  bool is_cte_definition = false;
};

// Skips whitespace and comments starting at `i`, returning the index of the
// first significant character (or `n`).
size_t SkipTrivia(const std::string& sql, size_t i) {
  const size_t n = sql.size();
  while (i < n) {
    char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 2 <= n) ? i + 2 : n;
      continue;
    }
    break;
  }
  return i;
}

// Given that `sql[open]` is the '(' of an `ident( ... )`, returns the index of
// the keyword that immediately follows the matching ')' (after trivia), if any.
// Used to recognize the `WITH name(cols) AS (...)` CTE-column-list pattern:
// after the column list, a CTE has the `AS` keyword. Respects nested parens,
// string literals and comments. Returns npos if no matching ')'.
size_t IndexAfterMatchingParen(const std::string& sql, size_t open) {
  const size_t n = sql.size();
  size_t i = open;
  int depth = 0;
  while (i < n) {
    char c = sql[i];
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 2 <= n) ? i + 2 : n;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      char quote = c;
      ++i;
      while (i < n) {
        if (sql[i] == quote) {
          if (i + 1 < n && sql[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
      if (depth == 0) {
        return SkipTrivia(sql, i + 1);
      }
    }
    ++i;
  }
  return std::string::npos;
}

// Top-level clause presence, used by the order-determinism guard. "Top level"
// = at parenthesis depth 0 (so an ORDER BY / LIMIT inside a subquery or window
// frame does not count as ordering the OUTER result). Computed by a small scan
// that skips comments + string/quoted literals and tracks paren depth.
struct ClauseInfo {
  bool has_order_by = false;
  bool has_limit = false;
  bool has_group_by = false;
};

ClauseInfo AnalyzeTopLevelClauses(const std::string& sql) {
  ClauseInfo info;
  const size_t n = sql.size();
  size_t i = 0;
  int depth = 0;
  std::string prev_word;  // previous identifier word (lowercased).
  auto is_ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  while (i < n) {
    char c = sql[i];
    // Whitespace does NOT reset prev_word (so `ORDER` and `BY` separated by
    // spaces are still seen as the keyword pair).
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 2 <= n) ? i + 2 : n;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      char q = c;
      ++i;
      while (i < n) {
        if (sql[i] == q) {
          if (i + 1 < n && sql[i + 1] == q) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      prev_word.clear();
      continue;
    }
    if (c == '(') {
      ++depth;
      ++i;
      prev_word.clear();
      continue;
    }
    if (c == ')') {
      if (depth > 0) {
        --depth;
      }
      ++i;
      prev_word.clear();
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      while (i < n && is_ident_char(sql[i])) {
        ++i;
      }
      std::string word = base::ToLower(sql.substr(start, i - start));
      if (depth == 0) {
        if (word == "by" && prev_word == "order") {
          info.has_order_by = true;
        } else if (word == "by" && prev_word == "group") {
          info.has_group_by = true;
        } else if (word == "limit") {
          info.has_limit = true;
        }
      }
      prev_word = word;
      continue;
    }
    ++i;
    prev_word.clear();
  }
  return info;
}

// Silent-divergence guard: COLUMN NAME. For an UNALIASED expression in the
// top-level SELECT list, SQLite names the result column after the source text
// (e.g. `COUNT(*)`, `COUNT(1)`, `a + b`) while DuckDB normalizes it (`count_star()`,
// `count(1)`, `(a + b)`). The diff-test goldens (CSV header row) encode SQLite's
// spelling, so routing such a query to DuckDB produces a SILENTLY WRONG header.
// This function detects the most common offender: a FUNCTION CALL `ident(...)`
// in the top-level projection (between the first top-level SELECT and its
// matching top-level FROM) that is NOT immediately followed by `AS`. (Aliased
// expressions are fine - the alias is the column name in both engines.)
// Returns true => the query has an unaliased expression column => fall back.
bool HasUnaliasedExprColumn(const std::string& sql);

// A minimal, dependency-free tokenizer over the (raw, user-supplied) SQL. It is
// intentionally simpler than the real PerfettoSQL/SQLite grammar: its only job
// is to feed the conservative, default-deny support predicate. Anything it can't
// confidently classify pushes the query toward INELIGIBLE (fall back), never
// toward over-claiming support.
//
// It recognizes: identifiers (incl. dotted `a.b`, only the first component is
// treated as a relation/function candidate), the FROM/JOIN context (so an
// identifier right after FROM or JOIN is a relation), function calls (identifier
// immediately followed by '('), string/quoted literals (skipped), comments
// (skipped), and statement separators (';').
std::vector<Token> Tokenize(const std::string& sql) {
  std::vector<Token> out;
  size_t i = 0;
  const size_t n = sql.size();
  bool prev_was_from_or_join = false;
  // CTE-column-list tracking. `cte_name_expected` is true when the next
  // identifier is the NAME of a CTE (right after the `WITH` keyword, and right
  // after a comma that separates CTE definitions). In that position an
  // identifier followed by `(...)  AS` is a CTE with a column list, NOT a
  // function call. We track the paren-nesting depth (relative to the WITH
  // clause) so a comma INSIDE a CTE body / column list does not falsely start a
  // new CTE name, and the WITH clause ends once we leave it.
  bool in_with_clause = false;
  bool cte_name_expected = false;
  int with_paren_depth = 0;
  auto is_ident_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  };
  auto is_ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  while (i < n) {
    char c = sql[i];
    // Whitespace.
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    // Line comment.
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    // Block comment.
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 2 <= n) ? i + 2 : n;
      continue;
    }
    // String / quoted literal: skip its contents entirely so identifiers inside
    // strings are never mistaken for relations.
    if (c == '\'' || c == '"' || c == '`') {
      char quote = c;
      ++i;
      while (i < n) {
        if (sql[i] == quote) {
          // Doubled quote = escaped quote inside the literal.
          if (i + 1 < n && sql[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      // A `"..."` token is a DIALECT divergence: SQLite treats a double-quoted
      // token that does not resolve to a column as a STRING literal (e.g.
      // `LN("as")` -> string), whereas DuckDB strictly treats it as a quoted
      // IDENTIFIER (and errors if no such column exists). Flag it so the
      // predicate can fall back rather than route a query that would error (or,
      // worse, bind differently) in DuckDB.
      if (quote == '"') {
        out.push_back({Token::kDoubleQuoted, "", false, false, false, false});
      }
      prev_was_from_or_join = false;
      continue;
    }
    // Semicolon = statement separator.
    if (c == ';') {
      out.push_back({Token::kSemicolon, "", false, false, false, false});
      ++i;
      prev_was_from_or_join = false;
      in_with_clause = false;
      cte_name_expected = false;
      with_paren_depth = 0;
      continue;
    }
    // Identifier (or keyword).
    if (is_ident_start(c)) {
      size_t start = i;
      while (i < n && is_ident_char(sql[i])) {
        ++i;
      }
      std::string word = sql.substr(start, i - start);
      std::string lower = base::ToLower(word);
      // Dotted reference `a.b`: skip the trailing `.b...` components; only the
      // first identifier is the relation/alias candidate.
      bool consumed_dot = false;
      while (i < n && sql[i] == '.') {
        consumed_dot = true;
        ++i;
        while (i < n && is_ident_char(sql[i])) {
          ++i;
        }
      }
      // Look ahead (skipping spaces) for a '(' to detect a function call.
      size_t j = i;
      while (j < n && std::isspace(static_cast<unsigned char>(sql[j]))) {
        ++j;
      }
      bool followed_by_paren = j < n && sql[j] == '(' && !consumed_dot;

      // Detect a CTE-column-list name: `WITH <name>(<cols>) AS (...)`. Only
      // when we are at a CTE-name position (right after WITH or a CTE-separating
      // comma) AND the identifier is followed by `(...)` whose matching `)` is
      // immediately followed by the `AS` keyword. This deliberately does NOT
      // fire for a function with an alias (e.g. `SELECT count(x) AS c`) because
      // that is not in a CTE-name position.
      bool is_cte_column_list = false;
      if (cte_name_expected && followed_by_paren && !IsSqlKeyword(lower)) {
        size_t after_paren = IndexAfterMatchingParen(sql, j);
        if (after_paren != std::string::npos && after_paren + 1 < n &&
            (sql[after_paren] == 'a' || sql[after_paren] == 'A') &&
            (sql[after_paren + 1] == 's' || sql[after_paren + 1] == 'S') &&
            (after_paren + 2 >= n ||
             !is_ident_char(sql[after_paren + 2]))) {
          is_cte_column_list = true;
        }
      }
      // This identifier DEFINES a CTE if we were expecting a CTE name here and
      // it is a real name (not a keyword). Captured before the slot is consumed.
      bool is_cte_definition = cte_name_expected && !IsSqlKeyword(lower);
      // The token consumes the CTE-name slot regardless of whether it had a
      // column list (a CTE may also be `name AS (...)` with no column list).
      if (cte_name_expected) {
        cte_name_expected = false;
      }
      if (lower == "with") {
        in_with_clause = true;
        with_paren_depth = 0;
        cte_name_expected = true;
      } else if (lower == "select" && with_paren_depth == 0) {
        // A top-level SELECT ends the WITH clause's CTE list: we are now in the
        // main query body, where a comma no longer starts a new CTE name.
        in_with_clause = false;
        cte_name_expected = false;
      }

      if (lower == "from" || lower == "join") {
        out.push_back({Token::kIdentifier, lower, false, false, false, false});
        prev_was_from_or_join = true;
        continue;
      }
      bool relation_pos = prev_was_from_or_join && !IsSqlKeyword(lower);
      out.push_back({Token::kIdentifier, lower, followed_by_paren, relation_pos,
                     is_cte_column_list, is_cte_definition});
      prev_was_from_or_join = false;
      continue;
    }
    // Any other punctuation. Track paren depth + commas so the CTE-name state
    // machine knows when one CTE definition ends and the next begins. A comma
    // at WITH-clause top level (paren depth 0) separates CTE definitions, so the
    // next identifier is again a CTE name.
    if (c == '(') {
      ++with_paren_depth;
    } else if (c == ')') {
      if (with_paren_depth > 0) {
        --with_paren_depth;
      }
    } else if (c == ',' && in_with_clause && with_paren_depth == 0) {
      cte_name_expected = true;
    }
    out.push_back({Token::kOther, "", false, false, false, false});
    ++i;
    prev_was_from_or_join = false;
  }
  return out;
}

bool HasUnaliasedExprColumn(const std::string& sql) {
  const size_t n = sql.size();
  size_t i = SkipTrivia(sql, 0);
  // Require the statement to start with a top-level SELECT (a CTE `WITH ...` is
  // not analyzed here - too complex; such queries are routed and, if the column
  // name diverges, the diff catches it; the dominant offenders are plain
  // `SELECT count(*) FROM ...`).
  auto is_kw = [&](size_t pos, const char* kw) {
    size_t len = std::strlen(kw);
    if (pos + len > n) {
      return false;
    }
    for (size_t k = 0; k < len; ++k) {
      if (std::tolower(static_cast<unsigned char>(sql[pos + k])) != kw[k]) {
        return false;
      }
    }
    // Must be a whole word.
    return pos + len >= n ||
           !(std::isalnum(static_cast<unsigned char>(sql[pos + len])) ||
             sql[pos + len] == '_');
  };
  if (!is_kw(i, "select")) {
    return false;
  }
  i += 6;  // past SELECT
  int depth = 0;
  auto is_ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  while (i < n) {
    i = SkipTrivia(sql, i);
    if (i >= n) {
      break;
    }
    char c = sql[i];
    if (c == '\'' || c == '"' || c == '`') {
      char q = c;
      ++i;
      while (i < n) {
        if (sql[i] == q) {
          if (i + 1 < n && sql[i + 1] == q) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == '(') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ')') {
      if (depth > 0) {
        --depth;
      }
      ++i;
      continue;
    }
    if (c == ';') {
      break;
    }
    if (is_ident_char(c) && (std::isalpha(static_cast<unsigned char>(c)) ||
                             c == '_')) {
      size_t start = i;
      while (i < n && is_ident_char(sql[i])) {
        ++i;
      }
      std::string word = base::ToLower(sql.substr(start, i - start));
      // A top-level FROM ends the projection list.
      if (depth == 0 && word == "from") {
        return false;
      }
      // An identifier (at projection top level) immediately followed by `(` is a
      // function call; if its matching `)` is NOT followed by `AS`, it is an
      // unaliased expression column => column-name divergence.
      if (depth == 0) {
        size_t after_ident = SkipTrivia(sql, i);
        if (after_ident < n && sql[after_ident] == '(' &&
            !IsSqlKeyword(word)) {
          size_t after_paren = IndexAfterMatchingParen(sql, after_ident);
          if (after_paren == std::string::npos) {
            return false;  // Unbalanced - let DuckDB/SQLite handle it.
          }
          // Is the next token the `AS` keyword (explicit alias)?
          bool aliased_as =
              after_paren + 1 < n &&
              (sql[after_paren] == 'a' || sql[after_paren] == 'A') &&
              (sql[after_paren + 1] == 's' || sql[after_paren + 1] == 'S') &&
              (after_paren + 2 >= n ||
               !is_ident_char(sql[after_paren + 2]));
          // Or an implicit alias (a bare identifier right after the `)`), e.g.
          // `count(*) cnt`. SQLite and DuckDB agree on an explicit name.
          bool aliased_implicit =
              !aliased_as && after_paren < n &&
              (std::isalpha(static_cast<unsigned char>(sql[after_paren])) ||
               sql[after_paren] == '_') &&
              !is_kw(after_paren, "from") && !is_kw(after_paren, "where") &&
              !is_kw(after_paren, "group") && !is_kw(after_paren, "order") &&
              !is_kw(after_paren, "limit");
          if (!aliased_as && !aliased_implicit) {
            return true;  // Unaliased function-call column.
          }
          i = after_paren;
          continue;
        }
      }
      continue;
    }
    ++i;
  }
  return false;
}

}  // namespace

DuckDbEngine::DuckDbEngine(StringPool* string_pool,
                           Resolver resolver,
                           ViewProvider view_provider)
    : string_pool_(string_pool),
      resolver_(std::move(resolver)),
      view_provider_(std::move(view_provider)) {}

DuckDbEngine::~DuckDbEngine() {
  // Destroy the provider BEFORE the database: the replacement scan + table
  // function hold a raw `this` pointer into the provider, so DuckDB must not
  // touch them after the provider is gone. Destroying the connection then the
  // database first guarantees no further callbacks fire.
  if (conn_) {
    duckdb_disconnect(&conn_);
  }
  if (db_) {
    duckdb_close(&db_);
  }
  provider_.reset();
}

base::Status DuckDbEngine::EnsureInitialized() {
  if (initialized_) {
    return base::OkStatus();
  }
  if (duckdb_open(nullptr, &db_) == DuckDBError) {
    return base::ErrStatus("DuckDbEngine: duckdb_open failed");
  }
  if (duckdb_connect(db_, &conn_) == DuckDBError) {
    duckdb_close(&db_);
    db_ = nullptr;
    return base::ErrStatus("DuckDbEngine: duckdb_connect failed");
  }
  // Single connection, single-threaded scans; keep DuckDB itself single
  // threaded too so the (non-thread-safe) StringPool reads stay serialized.
  duckdb_query(conn_, "SET threads TO 1;", nullptr);

  provider_ = std::make_unique<DuckDbTableProvider>(string_pool_, resolver_);
  RETURN_IF_ERROR(provider_->RegisterTableFunction(conn_));
  RETURN_IF_ERROR(provider_->RegisterReplacementScan(db_));

  // Register the first batch of pure scalar UDFs (math/regexp/unhex). Their
  // names become eligible in the support predicate via
  // registered_scalar_functions_.
  RETURN_IF_ERROR(
      RegisterScalarFunctions(conn_, &registered_scalar_functions_));

  initialized_ = true;
  return base::OkStatus();
}

void DuckDbEngine::SyncViews() {
  if (!view_provider_) {
    return;
  }
  // Mirror EVERY PerfettoSQL view into DuckDB's catalog, in creation order, so a
  // bare `FROM <view>` resolves through DuckDB's own view -> the view body's
  // `FROM __intrinsic_*` -> the replacement scan -> `__perfetto_df`. The stored
  // body is plain SQLite-dialect `CREATE VIEW <name> AS SELECT ...` (the
  // PerfettoSQL schema column list is not part of it). DuckDB binds the body
  // EAGERLY at CREATE VIEW time, so a view whose body uses SQLite-only dialect
  // (a D6 follow-up) or references a relation not yet wired into DuckDB fails to
  // create; we skip it (it stays unmirrored -> a query referencing it errors in
  // DuckDB and falls back / errors under disable_fallback), never faking it. It
  // is retried on the next call once its dependencies may exist.
  //
  // HISTORICAL NOTE: this used to be restricted to a hardcoded `IsSafeToMirror`
  // allowlist because mirroring the `stats` view appeared to "corrupt the
  // StringPool" (a garbage byte appended to stat names). That was a
  // MISDIAGNOSIS: the real bug was the DuckDB iterator returning a
  // non-NUL-terminated `duckdb_string_t` pointer as a C string, so consumers
  // (PrintStats `%s`) read past the end. Fixed in `duckdb_iterator_impl.cc`
  // (the iterator now copies into an owned NUL-terminated buffer); the allowlist
  // is therefore gone and every view mirrors safely.
  for (const auto& [name, create_view_sql] : view_provider_()) {
    std::string lower = base::ToLower(name);
    if (mirrored_views_.find(lower) != mirrored_views_.end()) {
      continue;  // Already mirrored.
    }
    duckdb_result res;
    if (duckdb_query(conn_, create_view_sql.c_str(), &res) == DuckDBError) {
      duckdb_destroy_result(&res);
      continue;
    }
    duckdb_destroy_result(&res);
    mirrored_views_.insert(lower);
  }
}

base::StatusOr<std::optional<DuckDbExecutionResult>>
DuckDbEngine::TryExecuteWholeQuery(const std::string& sql,
                                   bool disable_fallback,
                                   bool* ran_in_duckdb) {
  *ran_in_duckdb = false;

  // The honesty contract: a query is "ineligible" when it references anything
  // not yet available in DuckDB. With fallback enabled, ineligible => nullopt
  // (caller uses SQLite). With fallback disabled, ineligible => error, so a
  // measurement lane can prove the query truly ran in DuckDB.
  auto ineligible = [&](const std::string& why)
      -> base::StatusOr<std::optional<DuckDbExecutionResult>> {
    if (disable_fallback) {
      return base::ErrStatus(
          "DuckDB fallback disabled: query is not supported by the DuckDB "
          "engine (%s)",
          why.c_str());
    }
    return std::optional<DuckDbExecutionResult>(std::nullopt);
  };

  RETURN_IF_ERROR(EnsureInitialized());

  // Mirror any PerfettoSQL views created since the last query (after_eof
  // prelude, user INCLUDEs, runtime CREATE PERFETTO VIEW) so `FROM <view>`
  // resolves. Cheap: already-mirrored views are skipped.
  SyncViews();

  // --- Support predicate (cheap, conservative, default-deny). ---
  std::vector<Token> tokens = Tokenize(sql);

  // Require exactly one statement: reject a ';' that is followed by more
  // non-empty tokens (a trailing ';' is fine).
  for (size_t k = 0; k < tokens.size(); ++k) {
    if (tokens[k].kind != Token::kSemicolon) {
      continue;
    }
    for (size_t m = k + 1; m < tokens.size(); ++m) {
      if (tokens[m].kind != Token::kSemicolon) {
        return ineligible("more than one statement");
      }
    }
    break;
  }

  // Collect the names of CTEs defined in this query: a later `FROM <cte>`
  // references the local CTE, not an external relation, so the relation resolver
  // must not reject it.
  std::unordered_set<std::string> cte_names;
  for (const Token& t : tokens) {
    if (t.kind == Token::kIdentifier && t.is_cte_definition) {
      cte_names.insert(t.text);
    }
  }

  // Dialect guard: a `USING (col)` join coalesces the join column in SQLite,
  // but DuckDB resolves the duplicated column differently (it leaves both
  // qualified columns visible, so an unqualified reference binds ambiguously).
  // Until that divergence is validated/translated, a query with a USING join is
  // ineligible so it falls back cleanly rather than erroring inside DuckDB.
  // (This is distinct from the OLD bug, where `USING(` was mis-reported as a
  // missing *function*; it is now correctly recognized as the join clause and
  // treated as an unsupported dialect feature.)
  for (const Token& t : tokens) {
    if (t.kind == Token::kIdentifier && t.text == "using") {
      return ineligible("USING join clause (DuckDB column-resolution diverges)");
    }
  }

  // Dialect guard: a `"..."` double-quoted token is a STRING literal in SQLite
  // (when it doesn't match a column) but a quoted IDENTIFIER in DuckDB. A query
  // relying on the SQLite quirk (e.g. `LN("as")`) would error or bind
  // differently in DuckDB, so fall back rather than route it.
  for (const Token& t : tokens) {
    if (t.kind == Token::kDoubleQuoted) {
      return ineligible("double-quoted literal (SQLite string vs DuckDB ident)");
    }
  }

  // Dialect guard: a PerfettoSQL MACRO call is `name!(...)` (an identifier
  // immediately followed by `!`). DuckDB's parser does not understand the `!`
  // and raises a Parser Error. Macro expansion is a PerfettoSQL-frontend
  // feature; route such queries to SQLite. (We catch it here cheaply rather
  // than relying solely on the post-`duckdb_query` parse-error fallback, to
  // avoid a noisy DuckDB parse attempt.) Note `!=` is a valid operator, so only
  // a `!` that immediately follows an identifier character (and is not `!=`)
  // counts as a macro call.
  {
    const std::string& s = sql;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
      if (s[i] != '!') {
        continue;
      }
      if (s[i + 1] == '=') {
        continue;  // `!=` operator.
      }
      // Preceded by an identifier char => macro call `name!`.
      if (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) ||
                    s[i - 1] == '_')) {
        return ineligible("PerfettoSQL macro call (DuckDB cannot parse '!')");
      }
    }
  }

  bool saw_relation = false;
  bool saw_supported_function = false;
  bool saw_aggregate = false;
  for (const Token& t : tokens) {
    if (t.kind != Token::kIdentifier) {
      continue;
    }
    if (t.followed_by_paren) {
      // An identifier followed by '(' is a function call UNLESS it is:
      //  - a SQL keyword used with parentheses (e.g. `CAST(x AS INT)`,
      //    `USING(col)`, `VALUES(...)`) — structural, not a user function; or
      //  - the name of a CTE with a column list (`WITH data(a, b) AS (...)`) —
      //    `data` is a relation name, the parens hold column names.
      // Neither is checked against the function allowlist. A genuine function
      // call still must be in the allowlist (default-deny).
      if (IsSqlKeyword(t.text) || t.is_cte_column_list) {
        continue;
      }
      bool in_static_allowlist = BuiltinFunctionAllowlist().find(t.text) !=
                                 BuiltinFunctionAllowlist().end();
      bool is_registered_udf = registered_scalar_functions_.find(t.text) !=
                               registered_scalar_functions_.end();
      if (!in_static_allowlist && !is_registered_udf) {
        // The function allowlist is KEPT (unlike the relation check, which is
        // delegated to DuckDB's binder below). A call to a function DuckDB does
        // not have (an unported PerfettoSQL UDF / TVF, a custom intrinsic) would
        // ERROR inside DuckDB, but - critically - some unsupported constructs
        // could instead bind to a DuckDB builtin with DIVERGENT semantics and
        // produce SILENTLY WRONG output. Default-deny on functions guards that.
        return ineligible("function '" + t.text + "' not in allowlist");
      }
      saw_supported_function = true;
      if (t.text == "count" || t.text == "sum" || t.text == "min" ||
          t.text == "max" || t.text == "avg") {
        saw_aggregate = true;
      }
      continue;
    }
    if (t.after_from_or_join) {
      // RELATION POSITION. We DO NOT verify that the relation exists in DuckDB
      // anymore (the old per-table availability gate is removed): every static
      // dataframe and every PerfettoSQL view is registered/mirrored into
      // DuckDB's catalog up front (the replacement scan resolves any dataframe
      // name lazily, SyncViews mirrors every view), so DuckDB's BINDER is the
      // table oracle. A reference to a CTE defined in this query, or to any
      // relation DuckDB can bind, is fine; a reference to a relation DuckDB
      // cannot bind makes the eventual `duckdb_query` raise a Catalog/Binder
      // error, which we map to a clean fallback below (honest mode: error).
      // We only TRACK whether a FROM/JOIN relation was seen, to drive the
      // bare-SELECT gate.
      if (cte_names.find(t.text) != cte_names.end()) {
        continue;  // Local CTE: not an external relation.
      }
      saw_relation = true;
    }
  }
  // Gate: route a query iff it has a FROM/JOIN relation OR it is a bare (no-FROM)
  // statement that calls at least one allowlisted/registered function (e.g.
  // `SELECT ln(2)`). A no-FROM statement with neither (e.g. `SELECT 1`) still
  // falls back, to minimize behavioural drift from the SQLite path.
  if (!saw_relation && !saw_supported_function) {
    return ineligible("no DuckDB-backed relation or supported function");
  }

  // Silent-divergence guard: ROW ORDER. SQLite and DuckDB produce rows in
  // different engine-defined orders for a scan that is not fully ordered by a
  // top-level ORDER BY. The diff-test goldens encode SQLite's order, so routing
  // an under-ordered multi-row query to DuckDB yields SILENTLY WRONG (re-ordered)
  // output - and, worse, `LIMIT` without `ORDER BY` returns a DIFFERENT SET of
  // rows. This is exactly the `thread_slice_time_in_state` (`... LIMIT 10` with
  // no ORDER BY) landmine. Guard:
  //   - A query that scans a relation and has a top-level LIMIT but NO top-level
  //     ORDER BY is non-deterministic -> fall back.
  //   - A query that scans a relation, has NO top-level ORDER BY, and is NOT a
  //     single-row pure aggregate (an aggregate function with no GROUP BY -> one
  //     row, order-irrelevant) is order-divergence-prone -> fall back.
  // A query WITH a top-level ORDER BY is routed (residual tie-break divergence
  // is far rarer and is caught by the diff goldens if it bites). This trades a
  // few honest passes for ZERO order-divergence fallback regressions.
  if (saw_relation) {
    ClauseInfo clauses = AnalyzeTopLevelClauses(sql);
    if (!clauses.has_order_by) {
      if (clauses.has_limit) {
        return ineligible("LIMIT without ORDER BY (non-deterministic row set)");
      }
      bool single_row_aggregate = saw_aggregate && !clauses.has_group_by;
      if (!single_row_aggregate) {
        return ineligible(
            "multi-row scan without ORDER BY (engine-defined row order)");
      }
    }
  }

  // Silent-divergence guard: COLUMN NAME of an unaliased expression. SQLite
  // names such a column after its source text (`COUNT(*)`, `COUNT(1)`) while
  // DuckDB normalizes it (`count_star()`, `count(1)`); the CSV-header diff then
  // fails silently. Fall back when the top-level projection has an unaliased
  // function-call column.
  if (HasUnaliasedExprColumn(sql)) {
    return ineligible("unaliased expression column (column-name divergence)");
  }

  // --- Eligible: execute the whole query inside DuckDB. ---
  DuckDbExecutionResult exec;
  exec.last_statement_sql = sql;
  if (duckdb_query(conn_, sql.c_str(), &exec.result) == DuckDBError) {
    std::string err = duckdb_result_error(&exec.result);
    duckdb_destroy_result(&exec.result);
    // Now that the predicate no longer pre-verifies relations (DuckDB's binder
    // is the table oracle), a DuckDB ERROR here can be: a Catalog/Binder error
    // (unknown relation), a Parser error (residual SQLite/PerfettoSQL dialect),
    // or an execution-time error (e.g. a Conversion Error where SQLite's loose
    // typing would have silently coerced). In EVERY one of these cases falling
    // back to SQLite is correctness-SAFE: SQLite then produces the golden
    // result, and if SQLite ALSO errors the diff fails honestly. Crucially,
    // falling back on an ERROR never masks SILENT WRONG OUTPUT - that does not
    // error and is handled by the divergence guards above (order, USING,
    // double-quote, ceil/floor, function allowlist). So: ANY DuckDB error =>
    // ineligible => fall back (or, with fallback disabled, error honestly so the
    // measurement lane is trustworthy).
    return ineligible("DuckDB could not execute the query: " + err);
  }

  uint32_t col_count =
      static_cast<uint32_t>(duckdb_column_count(&exec.result));
  exec.column_count = col_count;
  exec.column_names.reserve(col_count);
  for (uint32_t c = 0; c < col_count; ++c) {
    const char* name = duckdb_column_name(&exec.result, c);
    exec.column_names.emplace_back(name ? name : "");
  }
  *ran_in_duckdb = true;
  return std::optional<DuckDbExecutionResult>(std::move(exec));
}

DuckDbEngine::SplitStatements DuckDbEngine::SplitTrailingStatement(
    const std::string& sql) {
  // Walk the SQL respecting comments + string/quoted literals, recording the
  // byte offset just past the LAST top-level ';' that is followed by more
  // non-whitespace, non-comment content. Everything up to and including that
  // ';' is the leading block; the remainder is the trailing statement.
  size_t i = 0;
  const size_t n = sql.size();
  size_t last_split = std::string::npos;  // index just AFTER a separating ';'.
  while (i < n) {
    char c = sql[i];
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) {
        ++i;
      }
      i = (i + 2 <= n) ? i + 2 : n;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`') {
      char quote = c;
      ++i;
      while (i < n) {
        if (sql[i] == quote) {
          if (i + 1 < n && sql[i + 1] == quote) {
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
      continue;
    }
    if (c == ';') {
      // Is there any real content after this ';'? If so it is a separator.
      size_t j = i + 1;
      // Skip whitespace and comments to find the next meaningful char.
      while (j < n) {
        if (std::isspace(static_cast<unsigned char>(sql[j]))) {
          ++j;
          continue;
        }
        if (sql[j] == '-' && j + 1 < n && sql[j + 1] == '-') {
          while (j < n && sql[j] != '\n') {
            ++j;
          }
          continue;
        }
        if (sql[j] == '/' && j + 1 < n && sql[j + 1] == '*') {
          j += 2;
          while (j + 1 < n && !(sql[j] == '*' && sql[j + 1] == '/')) {
            ++j;
          }
          j = (j + 2 <= n) ? j + 2 : n;
          continue;
        }
        break;
      }
      if (j < n) {
        last_split = i + 1;  // There is content after the ';'.
      }
    }
    ++i;
  }

  SplitStatements out;
  if (last_split == std::string::npos) {
    out.last = sql;
  } else {
    out.leading = sql.substr(0, last_split);
    out.last = sql.substr(last_split);
  }
  // Trim leading/trailing whitespace + a trailing ';' from the last statement.
  out.last = base::TrimWhitespace(out.last);
  while (!out.last.empty() && out.last.back() == ';') {
    out.last.pop_back();
    out.last = base::TrimWhitespace(out.last);
  }
  return out;
}

}  // namespace perfetto::trace_processor::duckdb_integration
