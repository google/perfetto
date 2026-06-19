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
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"
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
          // `DELEGATES TO __intrinsic_*`, so the surface name is what binds).
          // The delegated math surface (`sqrt`/`ln`/`exp`/`regexp_extract`/
          // `unhex`) is intentionally NOT here: those reach DuckDB as
          // `__intrinsic_*`, which has no DuckDB builtin (a real port, deferred).
          "abs", "round", "ceil", "floor", "trunc", "pow", "power",
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
  enum Kind { kIdentifier, kSemicolon, kOther };
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

  initialized_ = true;
  return base::OkStatus();
}

// The set of PerfettoSQL view names that are SAFE to mirror into DuckDB for the
// Wave-2 beachhead. Restricting to this curated set (rather than all created
// views) is a DELIBERATE, temporary safety measure: mirroring certain other
// prelude views (notably `stats`) corrupts the shared StringPool that the
// legacy SQLite engine later reads (observed: a garbage byte appended to stat
// name strings; root cause is a DuckDB-side interaction with the replacement
// scan during view binding and is tracked as a follow-up). These views are the
// ones the sched/thread stdlib surface needs and have been verified to mirror
// together without corruption. Generalising (and removing this allowlist once
// the corruption is root-caused) is a later wave.
static bool IsSafeToMirror(const std::string& lower_name) {
  static const std::unordered_set<std::string>* kSafe =
      new std::unordered_set<std::string>{
          "sched",   "sched_slice",
          "thread",  "thread_state",
          "cpu",     "process",
          "cpu_available_frequencies",
      };
  return kSafe->find(lower_name) != kSafe->end();
}

void DuckDbEngine::SyncViews() {
  if (!view_provider_) {
    return;
  }
  // Mirror the PerfettoSQL views into DuckDB's catalog, in creation order, so a
  // bare `FROM <view>` resolves through DuckDB's own view -> the view body's
  // `FROM __intrinsic_*` -> the replacement scan -> `__perfetto_df`. The stored
  // body is plain SQLite-dialect `CREATE VIEW <name> AS SELECT ...` (the
  // PerfettoSQL schema column list is not part of it). DuckDB binds the body
  // EAGERLY at CREATE VIEW time, so a view whose body uses SQLite-only dialect
  // (a D6 follow-up) or references a relation not yet wired into DuckDB (e.g.
  // `__intrinsic_stats`, which is a static table, not a dataframe) fails to
  // create; we skip it (it stays unmirrored -> any query referencing it is
  // deemed ineligible -> falls back / errors under disable_fallback), never
  // faking it. It is retried on the next call once its dependencies may exist.
  for (const auto& [name, create_view_sql] : view_provider_()) {
    std::string lower = base::ToLower(name);
    if (mirrored_views_.find(lower) != mirrored_views_.end()) {
      continue;  // Already mirrored.
    }
    if (!IsSafeToMirror(lower)) {
      continue;  // Not on the verified-safe allowlist (see IsSafeToMirror).
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

  bool saw_relation = false;
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
      if (BuiltinFunctionAllowlist().find(t.text) ==
          BuiltinFunctionAllowlist().end()) {
        return ineligible("function '" + t.text + "' not in allowlist");
      }
      continue;
    }
    if (t.after_from_or_join) {
      // A reference to a CTE defined in this same query resolves inside DuckDB;
      // it is not an external relation and does not, by itself, make the query
      // DuckDB-backed (a real dataframe inside the CTE body, if any, sets
      // saw_relation when the loop visits it).
      if (cte_names.find(t.text) != cte_names.end()) {
        continue;
      }
      // Relation: eligible if either (a) it is a PerfettoSQL view we mirrored
      // into DuckDB's catalog (DuckDB binds the view body, which transitively
      // reaches a dataframe through the replacement scan), or (b) it resolves
      // to a live dataframe via the read-through resolver. (Resolve also
      // snapshots the dataframe into the provider so the subsequent
      // duckdb_query sees it.) A mirrored view that turns out to be
      // unbindable (its body references something not yet available) will make
      // the eventual duckdb_query fail; that surfaces as an error, not a silent
      // fallback, which is the honest behaviour.
      bool is_mirrored_view =
          mirrored_views_.find(t.text) != mirrored_views_.end();
      // A dataframe is only usable if it is finalized: the provider snapshots it
      // via `CopyFinalized()`, which CHECK-fails on an unfinalized dataframe
      // (e.g. a runtime table mid-construction). An unfinalized table is treated
      // as not-yet-available -> ineligible -> fallback.
      const dataframe::Dataframe* df = resolver_ ? resolver_(t.text) : nullptr;
      bool is_dataframe = df != nullptr && df->finalized();
      if (!is_mirrored_view && !is_dataframe) {
        return ineligible("relation '" + t.text + "' not available in DuckDB");
      }
      saw_relation = true;
    }
  }
  // For the beachhead we only route queries that scan at least one of our
  // dataframes; a bare `SELECT <expr>` (no FROM) is left to SQLite for now.
  if (!saw_relation) {
    return ineligible("no DuckDB-backed relation referenced");
  }

  // --- Eligible: execute the whole query inside DuckDB. ---
  DuckDbExecutionResult exec;
  exec.last_statement_sql = sql;
  if (duckdb_query(conn_, sql.c_str(), &exec.result) == DuckDBError) {
    // A SUPPORTED query that genuinely failed: surface the error, do NOT fall
    // back (that would mask bugs).
    std::string err = duckdb_result_error(&exec.result);
    duckdb_destroy_result(&exec.result);
    return base::ErrStatus("DuckDB execution error: %s", err.c_str());
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
