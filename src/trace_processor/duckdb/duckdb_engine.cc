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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include "src/trace_processor/perfetto_sql/tokenizer/sqlite_tokenizer.h"
#include "src/trace_processor/sqlite/sql_source.h"

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

// A significant SQL token (whitespace and comments dropped) carrying its
// syntaqlite token TYPE and original text. The whole support predicate is driven
// off this real token stream: keywords (CAST/USING/AS/VALUES/WITH/...) get their
// OWN token types from the SQLite grammar, so they are never confused with an
// identifier the way the old hand-rolled character scanner did.
struct SigToken {
  int type;               // sql_token::k* constant.
  std::string str;        // original (case-preserving) token text (OWNED: the
                          // tokenizer's backing SqlSource does not outlive the
                          // returned vector).
  std::string lower;      // lowercased text (only populated for kId tokens).
  bool adjacent_to_prev;  // true if this token immediately follows the previous
                          // significant token with no intervening whitespace.
};

// Tokenizes `sql` with the real SQLite tokenizer (syntaqlite) and returns the
// significant tokens (whitespace and comments removed). `adjacent_to_prev`
// records byte-adjacency so a macro call `name!` (an identifier immediately
// followed by a `!` token) can be detected without re-scanning characters. The
// token text is COPIED into each SigToken because the tokenizer (and its backing
// SqlSource) is destroyed when this function returns.
std::vector<SigToken> TokenizeSql(const std::string& sql) {
  std::vector<SigToken> out;
  SqliteTokenizer tokenizer(SqlSource::FromTraceProcessorImplementation(sql));
  const char* prev_end = nullptr;
  for (auto t = tokenizer.Next(); !t.str.empty(); t = tokenizer.Next()) {
    if (t.token_type == sql_token::kSpace ||
        t.token_type == sql_token::kComment) {
      prev_end = t.str.data() + t.str.size();
      continue;
    }
    bool adjacent = prev_end != nullptr && t.str.data() == prev_end;
    std::string str(t.str);
    std::string lower =
        t.token_type == sql_token::kId ? base::ToLower(str) : std::string();
    out.push_back(SigToken{t.token_type, std::move(str), std::move(lower),
                           adjacent});
    prev_end = t.str.data() + t.str.size();
  }
  return out;
}

// True if `t` is a bare (unquoted) identifier: a real name candidate for a
// relation or a function call. A double-quoted or backtick-quoted identifier
// (which syntaqlite ALSO classifies as kId) is excluded - it is handled by the
// double-quote dialect guard, never treated as a function/relation name.
bool IsBareName(const SigToken& t) {
  return t.type == sql_token::kId && !t.str.empty() && t.str.front() != '"' &&
         t.str.front() != '`';
}

// True if `t` is a double-quoted (`"..."`) identifier. In SQLite a double-quoted
// token that does not resolve to a column is a STRING literal (e.g. `LN("as")`),
// whereas DuckDB strictly treats it as a quoted IDENTIFIER (and errors if no
// such column exists). The predicate falls back on these.
bool IsDoubleQuoted(const SigToken& t) {
  return t.type == sql_token::kId && !t.str.empty() && t.str.front() == '"';
}

// The outcome of the (cheap, conservative, default-deny) support predicate. When
// `ineligible_reason` is set the query must fall back to SQLite (or, in honest
// mode, error). When it is empty the query is eligible to be handed to DuckDB
// (DuckDB's binder remains the final oracle - any DuckDB error still falls
// back).
struct SupportDecision {
  std::optional<std::string> ineligible_reason;
};

// Drives the whole support predicate off the real syntaqlite token stream. It is
// intentionally testable in isolation (no live DuckDB): given the SQL plus the
// two function-eligibility sets, it returns the same eligibility outcome the
// engine previously computed with the hand-rolled scanner, just from real
// tokens.
//
// `builtin_allowlist` and `registered_udfs` are the function-name sets a call
// must match to be eligible (default-deny otherwise). They are passed in so the
// analysis stays a pure function of (SQL, eligible-function-names).
SupportDecision AnalyzeSupport(
    const std::string& sql,
    const std::unordered_set<std::string>& builtin_allowlist,
    const std::unordered_set<std::string>& registered_udfs) {
  auto ineligible = [](std::string why) {
    return SupportDecision{std::optional<std::string>(std::move(why))};
  };

  std::vector<SigToken> toks = TokenizeSql(sql);

  // Require exactly one statement: a ';' is only OK as a trailing terminator.
  for (size_t k = 0; k < toks.size(); ++k) {
    if (toks[k].type != sql_token::kSemi) {
      continue;
    }
    for (size_t m = k + 1; m < toks.size(); ++m) {
      if (toks[m].type != sql_token::kSemi) {
        return ineligible("more than one statement");
      }
    }
    break;
  }

  // Dialect guard: a `"..."` double-quoted token is a STRING literal in SQLite
  // (when it doesn't match a column) but a quoted IDENTIFIER in DuckDB.
  for (const SigToken& t : toks) {
    if (IsDoubleQuoted(t)) {
      return ineligible("double-quoted literal (SQLite string vs DuckDB ident)");
    }
  }

  // Dialect guard: USING (col) join. SQLite coalesces the join column; DuckDB
  // leaves both qualified columns visible, so an unqualified reference binds
  // ambiguously. `USING` is its OWN keyword token (kUsing) - it is never an
  // identifier, so this is a direct token-type test (the old code had to special
  // case the `using(` text to avoid a bogus "missing function" verdict).
  for (const SigToken& t : toks) {
    if (t.type == sql_token::kUsing) {
      return ineligible("USING join clause (DuckDB column-resolution diverges)");
    }
  }

  // Dialect guard: a PerfettoSQL MACRO call is `name!(...)`. The perfetto
  // syntaqlite dialect lexes a lone `!` (not `!=`) as its own kBang token (`!=`
  // is a single kNe token, never kBang). A kBang that is byte-adjacent to a
  // preceding identifier is a macro call (`foo!`). DuckDB cannot parse `!`, so
  // route such queries to the PerfettoSQL frontend.
  for (size_t k = 0; k < toks.size(); ++k) {
    const SigToken& t = toks[k];
    if (t.type == sql_token::kBang && t.adjacent_to_prev && k > 0 &&
        toks[k - 1].type == sql_token::kId) {
      return ineligible("PerfettoSQL macro call (DuckDB cannot parse '!')");
    }
  }

  // Collect CTE-defined names so a later `FROM <cte>` is recognized as a local
  // relation (not an external one). A CTE is introduced after `WITH` and after
  // each top-level (paren-depth-0) comma in the WITH clause: the next bare name
  // is the CTE name. The WITH clause ends at the first top-level SELECT.
  std::unordered_set<std::string> cte_names;
  {
    bool in_with = false;
    bool name_expected = false;
    int depth = 0;
    for (const SigToken& t : toks) {
      if (t.type == sql_token::kLp) {
        ++depth;
        continue;
      }
      if (t.type == sql_token::kRp) {
        if (depth > 0) {
          --depth;
        }
        continue;
      }
      if (!in_with) {
        // `WITH` introduces the CTE list. (kId named "with" never happens: WITH
        // is a keyword token, but we match defensively on the lowercase text via
        // the keyword token's str just in case.)
        if (base::CaseInsensitiveEqual(std::string(t.str), "with")) {
          in_with = true;
          name_expected = true;
          depth = 0;
        }
        continue;
      }
      if (depth == 0 && t.type == sql_token::kSelect) {
        in_with = false;
        name_expected = false;
        continue;
      }
      if (depth == 0 && t.type == sql_token::kComma) {
        name_expected = true;
        continue;
      }
      if (name_expected && IsBareName(t)) {
        cte_names.insert(t.lower);
        name_expected = false;
      }
    }
  }

  // Function-call + relation pass. A function call is a BARE identifier
  // immediately followed by `(` and NOT preceded by `.` (a dotted member like
  // `t.count` is a column, not a call). Because keywords (CAST/VALUES/...) and
  // quoted identifiers are NOT bare kId tokens, they never reach this test - the
  // old keyword/quote/CTE-column-list special cases fall out naturally.
  bool saw_relation = false;
  bool saw_supported_function = false;
  bool saw_aggregate = false;
  for (size_t k = 0; k < toks.size(); ++k) {
    const SigToken& t = toks[k];
    if (!IsBareName(t)) {
      continue;
    }
    bool dotted_member = k > 0 && toks[k - 1].type == sql_token::kDot;
    bool followed_by_paren =
        k + 1 < toks.size() && toks[k + 1].type == sql_token::kLp;

    // CTE-with-column-list: `WITH name(cols) AS (...)`. Here `name` is a bare id
    // followed by `(`, but the parens hold COLUMN NAMES, not function args - the
    // matching `)` is followed by `AS`. Recognize and skip (not a function).
    if (followed_by_paren && cte_names.find(t.lower) != cte_names.end()) {
      // Walk to the matching ')' and check the next token is AS.
      int depth = 0;
      size_t m = k + 1;
      for (; m < toks.size(); ++m) {
        if (toks[m].type == sql_token::kLp) {
          ++depth;
        } else if (toks[m].type == sql_token::kRp) {
          if (--depth == 0) {
            break;
          }
        }
      }
      if (m + 1 < toks.size() && toks[m + 1].type == sql_token::kAs) {
        continue;  // CTE column list, not a function call.
      }
    }

    if (followed_by_paren && !dotted_member) {
      bool in_static_allowlist =
          builtin_allowlist.find(t.lower) != builtin_allowlist.end();
      bool is_registered_udf =
          registered_udfs.find(t.lower) != registered_udfs.end();
      if (!in_static_allowlist && !is_registered_udf) {
        // Default-deny: a call to a function DuckDB lacks would ERROR (safe
        // fallback), but a construct that binds to a DuckDB builtin with
        // DIVERGENT semantics could produce SILENTLY WRONG output. Guard it.
        return ineligible("function '" + t.lower + "' not in allowlist");
      }
      saw_supported_function = true;
      if (t.lower == "count" || t.lower == "sum" || t.lower == "min" ||
          t.lower == "max" || t.lower == "avg") {
        saw_aggregate = true;
      }
      continue;
    }

    // RELATION POSITION: a bare name right after FROM or JOIN. The predicate
    // does NOT verify the relation exists - DuckDB's binder is the table oracle
    // (every dataframe + view is registered/mirrored up front); a reference it
    // cannot bind makes `duckdb_query` error and we fall back. We only TRACK
    // whether a relation was seen (to drive the bare-SELECT gate), skipping
    // local CTE references and dotted members.
    if (!dotted_member && k > 0 &&
        (toks[k - 1].type == sql_token::kFrom ||
         toks[k - 1].type == sql_token::kJoin)) {
      if (cte_names.find(t.lower) == cte_names.end()) {
        saw_relation = true;
      }
    }
  }

  // Gate: route a query iff it has a FROM/JOIN relation OR it is a bare (no-FROM)
  // statement calling at least one allowlisted/registered function (e.g.
  // `SELECT ln(2)`). A no-FROM statement with neither (e.g. `SELECT 1`) falls
  // back, to minimize behavioural drift from the SQLite path.
  if (!saw_relation && !saw_supported_function) {
    return ineligible("no DuckDB-backed relation or supported function");
  }

  // Top-level clause presence (paren depth 0) for the row-order and column-name
  // guards. `kOrder kBy` => ORDER BY; `kGroup kBy` => GROUP BY; `kLimit` =>
  // LIMIT, all only at depth 0 (a clause inside a subquery does not order the
  // OUTER result).
  bool has_order_by = false;
  bool has_group_by = false;
  bool has_limit = false;
  {
    int depth = 0;
    for (size_t k = 0; k < toks.size(); ++k) {
      const SigToken& t = toks[k];
      if (t.type == sql_token::kLp) {
        ++depth;
      } else if (t.type == sql_token::kRp) {
        if (depth > 0) {
          --depth;
        }
      } else if (depth == 0) {
        if (t.type == sql_token::kBy && k > 0) {
          if (toks[k - 1].type == sql_token::kOrder) {
            has_order_by = true;
          } else if (toks[k - 1].type == sql_token::kGroup) {
            has_group_by = true;
          }
        } else if (t.type == sql_token::kLimit) {
          has_limit = true;
        }
      }
    }
  }

  // Silent-divergence guard: ROW ORDER. SQLite and DuckDB order rows
  // differently for an under-ordered scan; the goldens encode SQLite's order.
  //   - relation + top-level LIMIT but NO top-level ORDER BY => non-deterministic
  //     row SET (the `... LIMIT 10` landmine) => fall back.
  //   - relation + NO top-level ORDER BY and NOT a single-row pure aggregate
  //     (aggregate with no GROUP BY) => order-divergence-prone => fall back.
  if (saw_relation && !has_order_by) {
    if (has_limit) {
      return ineligible("LIMIT without ORDER BY (non-deterministic row set)");
    }
    bool single_row_aggregate = saw_aggregate && !has_group_by;
    if (!single_row_aggregate) {
      return ineligible(
          "multi-row scan without ORDER BY (engine-defined row order)");
    }
  }

  // Silent-divergence guard: COLUMN NAME of an unaliased expression. SQLite names
  // an unaliased top-level projection column after its source text (`COUNT(*)`,
  // `COUNT(1)`) while DuckDB normalizes it (`count_star()`, `count(1)`); the
  // CSV-header diff then fails silently. Detect a function-call column in the
  // top-level projection (between the first top-level SELECT and its matching
  // top-level FROM) NOT followed by an alias (an `AS` keyword or a bare implicit
  // alias name). Only analyzed for a plain leading SELECT (a leading WITH is
  // routed and caught by the diff if it bites, as before).
  if (!toks.empty() && toks[0].type == sql_token::kSelect) {
    int depth = 0;
    for (size_t k = 1; k < toks.size(); ++k) {
      const SigToken& t = toks[k];
      if (t.type == sql_token::kLp) {
        ++depth;
        continue;
      }
      if (t.type == sql_token::kRp) {
        if (depth > 0) {
          --depth;
        }
        continue;
      }
      if (depth != 0) {
        continue;
      }
      if (t.type == sql_token::kFrom || t.type == sql_token::kSemi) {
        break;  // End of the top-level projection list.
      }
      // A bare-name function call in projection position.
      if (IsBareName(t) && k + 1 < toks.size() &&
          toks[k + 1].type == sql_token::kLp &&
          !(k > 0 && toks[k - 1].type == sql_token::kDot)) {
        // Find the matching ')'.
        int d = 0;
        size_t m = k + 1;
        for (; m < toks.size(); ++m) {
          if (toks[m].type == sql_token::kLp) {
            ++d;
          } else if (toks[m].type == sql_token::kRp) {
            if (--d == 0) {
              break;
            }
          }
        }
        if (m >= toks.size()) {
          break;  // Unbalanced - let DuckDB/SQLite handle it.
        }
        // Is the call aliased? Either an explicit `AS`, or an implicit alias
        // (a bare name) right after the `)`. Both spell the column name the
        // same way in DuckDB and SQLite.
        const SigToken* after = (m + 1 < toks.size()) ? &toks[m + 1] : nullptr;
        bool aliased_as = after && after->type == sql_token::kAs;
        bool aliased_implicit = after && IsBareName(*after);
        if (!aliased_as && !aliased_implicit) {
          return ineligible(
              "unaliased expression column (column-name divergence)");
        }
        k = m;  // Skip past the call.
      }
    }
  }

  return SupportDecision{};
}

}  // namespace

namespace internal {
std::optional<std::string> AnalyzeSupportForTesting(
    const std::string& sql,
    const std::unordered_set<std::string>& builtin_allowlist,
    const std::unordered_set<std::string>& registered_udfs) {
  return AnalyzeSupport(sql, builtin_allowlist, registered_udfs)
      .ineligible_reason;
}
}  // namespace internal

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
  // Driven entirely off the real syntaqlite token stream (see AnalyzeSupport):
  // statement count, the USING / double-quote / macro dialect guards, the
  // function allowlist, the relation/CTE bookkeeping, and the row-order +
  // column-name divergence guards. Keywords and quoted identifiers carry their
  // own token types, so the old keyword/quote/CTE-column-list special cases are
  // gone - they fall out of proper token classification.
  SupportDecision decision = AnalyzeSupport(sql, BuiltinFunctionAllowlist(),
                                            registered_scalar_functions_);
  if (decision.ineligible_reason) {
    return ineligible(*decision.ineligible_reason);
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
