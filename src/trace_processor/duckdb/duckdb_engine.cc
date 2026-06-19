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
          "count", "sum", "min", "max", "avg",
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
};

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
      out.push_back({Token::kSemicolon, "", false, false});
      ++i;
      prev_was_from_or_join = false;
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

      if (lower == "from" || lower == "join") {
        out.push_back({Token::kIdentifier, lower, false, false});
        prev_was_from_or_join = true;
        continue;
      }
      bool relation_pos = prev_was_from_or_join && !IsSqlKeyword(lower);
      out.push_back(
          {Token::kIdentifier, lower, followed_by_paren, relation_pos});
      prev_was_from_or_join = false;
      continue;
    }
    // Any other punctuation.
    out.push_back({Token::kOther, "", false, false});
    ++i;
    prev_was_from_or_join = false;
  }
  return out;
}

}  // namespace

DuckDbEngine::DuckDbEngine(StringPool* string_pool, Resolver resolver)
    : string_pool_(string_pool), resolver_(std::move(resolver)) {}

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

  bool saw_relation = false;
  for (const Token& t : tokens) {
    if (t.kind != Token::kIdentifier) {
      continue;
    }
    if (t.followed_by_paren) {
      // Function call: must be in the allowlist.
      if (BuiltinFunctionAllowlist().find(t.text) ==
          BuiltinFunctionAllowlist().end()) {
        return ineligible("function '" + t.text + "' not in allowlist");
      }
      continue;
    }
    if (t.after_from_or_join) {
      // Relation: must resolve to a live dataframe via the read-through
      // resolver. (Resolve also snapshots it into the provider so the
      // subsequent duckdb_query sees it.)
      if (!resolver_ || resolver_(t.text) == nullptr) {
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

}  // namespace perfetto::trace_processor::duckdb_integration
