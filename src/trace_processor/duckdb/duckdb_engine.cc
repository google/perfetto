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

#include <algorithm>
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
          // Null-handling conditionals. These are SQL-standard and resolve to the
          // SAME semantics in DuckDB as in SQLite: `coalesce`/`ifnull` are
          // parsed into a COALESCE operator (first non-NULL argument); `nullif`
          // into `CASE WHEN a=b THEN NULL ELSE a END`. DuckDB transforms them at
          // parse time (transform_function.cpp / transform_operator.cpp), so they
          // bind identically regardless of argument types. 1:1 with SQLite.
          //
          // `iif`/`format`/`hex` are deliberately EXCLUDED: `iif` is not a DuckDB
          // builtin (DuckDB has no `iif`; it uses `CASE WHEN`), so it would error
          // into the fallback; `format` uses Python `{}`-style formatting in
          // DuckDB (not SQLite's `%`-style printf), so `format('%.5f', x)`
          // diverges; `hex(INTEGER)` hexes the integer VALUE in DuckDB whereas
          // SQLite hexes the integer's TEXT rendering - a value divergence.
          "coalesce", "ifnull", "nullif",
          // `group_concat(X)` is a DuckDB-native aggregate (alias of `string_agg`)
          // whose default separator is `,`, matching SQLite. Concatenation ORDER
          // within a group is engine-defined in both, so an unordered
          // `group_concat` over a tie can diverge - those land in the known-bad
          // ledger (TIE_BREAK), not here.
          "group_concat",
          // Window functions. These are SQL-standard and DuckDB-native with the
          // same frame/ordering semantics as SQLite, so a windowed call binds
          // and evaluates identically (the divergences that remain are tie-break
          // ordering of equal-key rows, which land in the known-bad ledger, not
          // here). Adding them to the allowlist is monotonic for the honest lane.
          "row_number", "rank", "dense_rank", "ntile", "lag", "lead",
          "first_value", "last_value", "nth_value", "cume_dist",
          "percent_rank",
          // String builtins that are 1:1 with SQLite for ASCII inputs (the test
          // corpus): `length` (char count), `substr`/`substring` (1-based, with
          // negative-from-end support in both), `instr` (1-based, 0 on miss),
          // `replace`, `trim`/`ltrim`/`rtrim` (default whitespace), `lower`/
          // `upper` (ASCII-fold; non-ASCII unicode-folding divergences land in
          // known-bad), `reverse`. Non-matching cases are cataloged, not guarded.
          "length", "substr", "substring", "instr", "replace", "trim",
          "ltrim", "rtrim", "lower", "upper", "reverse",
          // Aggregates beyond the beachhead that are SQL-standard and match
          // SQLite: min/max/sum/avg/count already above; total (SUM-as-double),
          // and the statistical aggregates DuckDB shares.
          "total",
          // `iif(c,a,b)` is registered as a DuckDB MACRO (CASE WHEN c THEN a ELSE
          // b END) in RegisterScalarFunctions, so it binds with exact SQLite
          // semantics. Listed here so the support predicate treats it as eligible
          // (it is also in registered_scalar_functions_ via the macro path).
          "iif",
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
    const std::unordered_set<std::string>& registered_udfs,
    const std::unordered_set<std::string>& table_macros) {
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

  // POLICY SHIFT ("list, don't guard"): the double-quote and USING guards were
  // RELAXED. Both were preemptive dialect rejections; in practice DuckDB handles
  // the overwhelming majority of these constructs identically to SQLite:
  //   - A `"..."` double-quoted token that names a real column binds as that
  //     column in DuckDB exactly as in SQLite. Only the (rare in this corpus)
  //     "double-quote as STRING LITERAL" case diverges; when it does, DuckDB
  //     raises a Binder error (no such column) and the ANY-DuckDB-error rule
  //     falls back. The genuinely-silent cases (a literal that coincidentally
  //     matches a column name) are cataloged in the known-bad ledger.
  //   - Modern DuckDB follows the SQL standard for `JOIN ... USING (col)`: the
  //     join column is COALESCED into a single output column and an unqualified
  //     reference is unambiguous - matching SQLite. (The original guard's claim
  //     that DuckDB "leaves both qualified columns visible" no longer holds.)
  //     Divergent cases surface as a Binder error (fall back) or a value
  //     divergence (cataloged), not silent corruption of the common case.
  // Letting these run converts a large block of needlessly-suppressed queries
  // into genuine DuckDB passes.

  // NOTE: the PerfettoSQL MACRO `name!(...)` pre-check was DROPPED. DuckDB cannot
  // parse `!`, so such a query raises a Parser error in `duckdb_query` and the
  // ANY-DuckDB-error-falls-back rule already routes it correctly. A dedicated
  // token pre-check was therefore redundant (it only avoided a noisy parse
  // attempt) - per the "list, don't guard" policy we let DuckDB reject it.

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
      // A mirrored table macro is invoked as a table-valued reference
      // (`FROM name(args)` / `JOIN name(args)`). It is eligible like a mirrored
      // view (DuckDB binds the macro body, which references already-mirrored
      // tables/views); it also counts as a relation for the bare-SELECT gate.
      if (table_macros.find(t.lower) != table_macros.end()) {
        saw_relation = true;
        continue;
      }
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

  // POLICY SHIFT ("list, don't guard"): the ROW-ORDER guard was RELAXED. It used
  // to force a fallback whenever a query scanned a relation with no top-level
  // ORDER BY (and on LIMIT-without-ORDER-BY), on the theory that SQLite and
  // DuckDB order an under-specified scan differently. In practice DuckDB's scan
  // over the dataframe cursor is STABLE and, for many such queries, byte-matches
  // the SQLite golden - those are genuine DuckDB passes the guard was needlessly
  // suppressing. We now LET these queries run in DuckDB: the deterministically
  // matching ones PASS, and the few that genuinely diverge on tie-break /
  // arbitrary row order are recorded in duckdb_known_bad_tests.txt rather than
  // guarded. (`saw_aggregate` is consequently unused; kept the relation/CTE
  // bookkeeping for the bare-SELECT gate above.)
  (void)saw_aggregate;

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
    const std::unordered_set<std::string>& registered_udfs,
    const std::unordered_set<std::string>& table_macros) {
  return AnalyzeSupport(sql, builtin_allowlist, registered_udfs, table_macros)
      .ineligible_reason;
}
}  // namespace internal

DuckDbEngine::DuckDbEngine(StringPool* string_pool,
                           Resolver resolver,
                           ViewProvider view_provider,
                           FunctionProvider function_provider)
    : string_pool_(string_pool),
      resolver_(std::move(resolver)),
      view_provider_(std::move(view_provider)),
      function_provider_(std::move(function_provider)) {}

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

  // Match SQLite's NULL ordering. SQLite treats NULL as smaller than any other
  // value, so an unqualified `ORDER BY x` (ASC) sorts NULLs FIRST and `ORDER BY
  // x DESC` sorts NULLs LAST. DuckDB defaults to NULLS_LAST regardless of
  // direction, which diverges from the SQLite goldens. DuckDB exposes exactly
  // the SQLite-matching policy as `NULLS_FIRST_ON_ASC_LAST_ON_DESC` (verified in
  // buildtools/duckdb DefaultOrderByNullType enum). Setting it makes
  // under-specified NULL tie ordering byte-identical to the goldens.
  duckdb_query(
      conn_, "SET default_null_order='nulls_first_on_asc_last_on_desc';",
      nullptr);

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

namespace {

// Rewrites the SQLite `$arg` bind placeholders in a RETURNS TABLE function body
// into the bare parameter names a DuckDB table macro uses. For each parameter
// `arg` we replace every occurrence of the token `$arg` (the `$` immediately
// followed by the exact name, and NOT followed by another identifier char so
// `$ts` does not match inside `$tsx`) with `arg`. Done longest-name-first so a
// parameter that is a prefix of another (`$ts` vs `$ts_end`) cannot mis-rewrite.
std::string RewriteDollarParams(const std::string& body,
                                std::vector<std::string> arg_names) {
  auto is_ident_char = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  std::sort(arg_names.begin(), arg_names.end(),
            [](const std::string& a, const std::string& b) {
              return a.size() > b.size();
            });
  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < body.size();) {
    if (body[i] == '$') {
      bool matched = false;
      for (const std::string& name : arg_names) {
        if (name.empty()) {
          continue;
        }
        size_t end = i + 1 + name.size();
        if (body.compare(i + 1, name.size(), name) == 0 &&
            (end >= body.size() || !is_ident_char(body[end]))) {
          out.append(name);
          i = end;
          matched = true;
          break;
        }
      }
      if (matched) {
        continue;
      }
    }
    out.push_back(body[i]);
    ++i;
  }
  return out;
}

}  // namespace

void DuckDbEngine::SyncTableFunctions() {
  if (!function_provider_) {
    return;
  }
  // Mirror each stdlib RETURNS TABLE function as a DuckDB table macro so a bare
  // `FROM name(args)` resolves through DuckDB's own macro -> the body's
  // `FROM <table/view>` -> the replacement scan. The body is post-macro-expansion
  // SQLite/PerfettoSQL dialect; DuckDB binds it EAGERLY at CREATE MACRO time, so
  // a body using SQLite-only dialect or an `__intrinsic_*` table-pointer ABI
  // (e.g. interval_intersect) fails to create and stays unmirrored. We never
  // fake it: a query calling an unmirrored function errors in DuckDB and falls
  // back (or errors under disable_fallback).
  for (const TableFunction& fn : function_provider_()) {
    std::string lower = base::ToLower(fn.name);
    if (mirrored_table_macros_.find(lower) != mirrored_table_macros_.end()) {
      continue;  // Already mirrored.
    }
    std::string params;
    for (size_t i = 0; i < fn.arg_names.size(); ++i) {
      if (i != 0) {
        params += ", ";
      }
      params += fn.arg_names[i];
    }
    std::string body = RewriteDollarParams(fn.body_sql, fn.arg_names);
    std::string create = "CREATE MACRO " + fn.name + "(" + params +
                         ") AS TABLE (" + body + ")";
    duckdb_result res;
    if (duckdb_query(conn_, create.c_str(), &res) == DuckDBError) {
      duckdb_destroy_result(&res);
      continue;
    }
    duckdb_destroy_result(&res);
    mirrored_table_macros_.insert(lower);
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

  // Mirror any stdlib RETURNS TABLE functions created since the last query as
  // DuckDB table macros so `FROM name(args)` resolves. Cheap: already-mirrored
  // macros are skipped.
  SyncTableFunctions();

  // --- Support predicate (cheap, conservative, default-deny). ---
  // Driven entirely off the real syntaqlite token stream (see AnalyzeSupport):
  // statement count, the USING / double-quote / macro dialect guards, the
  // function allowlist, the relation/CTE bookkeeping, and the row-order +
  // column-name divergence guards. Keywords and quoted identifiers carry their
  // own token types, so the old keyword/quote/CTE-column-list special cases are
  // gone - they fall out of proper token classification.
  SupportDecision decision =
      AnalyzeSupport(sql, BuiltinFunctionAllowlist(),
                     registered_scalar_functions_, mirrored_table_macros_);
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
