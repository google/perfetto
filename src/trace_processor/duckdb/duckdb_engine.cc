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
#include "src/trace_processor/duckdb/dominator_tree_function.h"
#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"
#include "src/trace_processor/duckdb/graph_function.h"
#include "src/trace_processor/duckdb/interval_intersect_function.h"
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
          // `format` is SQLite's C-style printf. It is rewritten to DuckDB's
          // C-style `printf` before execution (RewriteFormatToPrintf); DuckDB's
          // own `format` is Python `{}`-style and would diverge, so it is the
          // rewrite - not a direct bind - that makes this eligible.
          "format",
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
          // `unicode(s)` (codepoint of the first char) is DuckDB-native with
          // identical semantics. `char(x)` is SQLite-only: its single-argument
          // form is rewritten to DuckDB's `chr(x)` (RewriteCharToChr); the
          // variadic form is left to fall back. Allowlisted here so the
          // predicate accepts char/unicode-using queries (e.g. the graph
          // lengauer examples). `chr` is allowed for the rewritten output.
          "unicode", "char", "chr",
          // Aggregates beyond the beachhead that are SQL-standard and match
          // SQLite: min/max/sum/avg/count already above; total (SUM-as-double),
          // and the statistical aggregates DuckDB shares.
          "total",
          // `iif(c,a,b)` is registered as a DuckDB MACRO (CASE WHEN c THEN a ELSE
          // b END) in RegisterScalarFunctions, so it binds with exact SQLite
          // semantics. Listed here so the support predicate treats it as eligible
          // (it is also in registered_scalar_functions_ via the macro path).
          "iif",
          // `greatest`/`least` are DuckDB-native variadic max/min. They appear
          // only in engine-GENERATED SQL (the _interval_intersect! rewrite), so
          // their (DuckDB-exact) semantics are controlled by us, not user input.
          "greatest", "least",
          // The native interval_intersect aggregate + combiner and `unnest`,
          // emitted only by the engine-GENERATED _interval_intersect! rewrite
          // (RewriteIntervalIntersectMacro). User input cannot reach these.
          "__intrinsic_ii_agg", "__intrinsic_ii_combine", "unnest",
          // The native graph BFS/DFS aggregates + combiners, emitted only by
          // the engine-GENERATED graph_reachable_bfs!/_dfs! rewrite
          // (RewriteGraphReachableMacro). User input cannot reach these.
          "__intrinsic_graph_agg", "__intrinsic_int_array_agg",
          "__intrinsic_graph_bfs", "__intrinsic_graph_dfs",
          // Native dominator-tree aggregate, emitted only by the engine-
          // GENERATED graph_dominator_tree! rewrite (RewriteGraphDominatorMacro).
          "__intrinsic_dominator_tree",
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

  // POLICY SHIFT ("list, don't guard"): the bare-SELECT gate was RELAXED. It
  // used to reject a no-FROM statement that called no allowlisted function (e.g.
  // `SELECT 1`, or a macro-expanded `SELECT (SELECT 123 - 100)`), to minimize
  // drift. But such constant/expression SELECTs evaluate identically in DuckDB
  // and SQLite (integer division included - DuckDB does integer division for
  // integer operands), so routing them is safe; any genuinely-divergent case
  // surfaces as a DuckDB error (fall back) or a value divergence (cataloged).
  // This is what lets a PerfettoSQL MACRO that expands to a bare SELECT run in
  // DuckDB. A bare SELECT calling an UNALLOWLISTED function is still rejected
  // above by the function allowlist, so this only admits literal/operator/
  // allowlisted-function expressions.
  (void)saw_relation;
  (void)saw_supported_function;

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

  // POLICY SHIFT ("list, don't guard"): the COLUMN-NAME divergence guard was
  // REPLACED by a post-execution column-name OVERRIDE (see
  // ComputeColumnNameOverrides). SQLite names an unaliased top-level projection
  // column after its source text (`MAX(id)`, `COUNT(*)`) while DuckDB
  // canonicalizes it (`max(id)`, `count_star()`); rather than reject these
  // queries, the engine runs them in DuckDB and rewrites the affected output
  // column names to the SQLite source text. This converts the previously-
  // rejected (honest-fail) queries into passes.
  return SupportDecision{};
}

// Reconstructs the source text of the token range [begin, end) (end exclusive)
// from the significant-token stream, inserting a single space before each token
// that was NOT byte-adjacent to its predecessor. For tightly-written
// projections (`MAX(id)`, `COUNT(*)`) this reproduces SQLite's column-name text
// exactly; multiple-space / comment-laden spellings collapse to single spaces
// (rare in the corpus, and only ever a column-header cosmetic).
std::string ReconstructText(const std::vector<SigToken>& toks,
                            size_t begin,
                            size_t end) {
  std::string out;
  for (size_t k = begin; k < end; ++k) {
    if (k != begin && !toks[k].adjacent_to_prev) {
      out.push_back(' ');
    }
    out.append(toks[k].str);
  }
  return out;
}

// Computes per-output-column name OVERRIDES so a DuckDB result's column headers
// match SQLite's. SQLite names an unaliased top-level projection column after
// its source text; DuckDB canonicalizes function calls (lowercased, `count(*)`
// -> `count_star()`). For each top-level projection that is EXACTLY a bare
// function call with NO alias and NO trailing tokens (so there is no ambiguity
// about whether a trailing bare name is an implicit alias), the override is the
// reconstructed source text; for every other projection (simple column refs,
// aliased exprs, arithmetic, implicit-aliased calls) the slot is left empty
// (nullopt) so DuckDB's own name is kept - those already match or are too
// ambiguous to safely rewrite.
//
// Returns an EMPTY vector (meaning "do not override anything") when positional
// mapping from projection items to output columns is not safe: the query does
// not begin with SELECT, a top-level set operation (UNION/INTERSECT/EXCEPT)
// appears before FROM, or any projection item contains a top-level `*` (which
// expands to an unknown number of columns). In those cases the caller keeps
// DuckDB's names verbatim.
std::vector<std::optional<std::string>> ComputeColumnNameOverrides(
    const std::string& sql) {
  std::vector<SigToken> toks = TokenizeSql(sql);
  std::vector<std::optional<std::string>> empty;
  if (toks.empty() || toks[0].type != sql_token::kSelect) {
    return empty;
  }

  // Find the end of the top-level projection list: the first top-level FROM (or
  // a clause keyword / semicolon / EOF when there is no FROM). Bail on a
  // top-level set operation (positional column mapping would be unsafe).
  size_t proj_end = toks.size();
  {
    int depth = 0;
    for (size_t k = 1; k < toks.size(); ++k) {
      int tt = toks[k].type;
      if (tt == sql_token::kLp) {
        ++depth;
        continue;
      }
      if (tt == sql_token::kRp) {
        if (depth > 0) {
          --depth;
        }
        continue;
      }
      if (depth != 0) {
        continue;
      }
      if (tt == sql_token::kFrom || tt == sql_token::kSemi) {
        proj_end = k;
        break;
      }
    }
  }

  // Split [1, proj_end) into items by top-level commas; collect each item's
  // token range. Bail if an item has a top-level `*` (column expansion).
  std::vector<std::pair<size_t, size_t>> items;  // [begin, end) per item.
  {
    int depth = 0;
    size_t item_begin = 1;
    auto push_item = [&](size_t e) {
      if (e > item_begin) {
        items.emplace_back(item_begin, e);
      }
    };
    for (size_t k = 1; k < proj_end; ++k) {
      int tt = toks[k].type;
      if (tt == sql_token::kLp) {
        ++depth;
      } else if (tt == sql_token::kRp) {
        if (depth > 0) {
          --depth;
        }
      } else if (depth == 0 && tt == sql_token::kComma) {
        push_item(k);
        item_begin = k + 1;
      } else if (depth == 0 && toks[k].str == "*") {
        return empty;  // SELECT * / expr.* : unsafe to map positionally.
      }
    }
    push_item(proj_end);
  }
  if (items.empty()) {
    return empty;
  }

  std::vector<std::optional<std::string>> overrides(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    size_t b = items[i].first;
    size_t e = items[i].second;  // exclusive.
    // Is the item EXACTLY a bare function call `name ( ... )` with the matching
    // ')' as its final token (no alias, no trailing tokens)?
    if (e - b < 3) {
      continue;  // Too short to be name '(' ')'.
    }
    if (!IsBareName(toks[b]) || toks[b + 1].type != sql_token::kLp) {
      continue;
    }
    int d = 0;
    size_t m = b + 1;
    for (; m < e; ++m) {
      if (toks[m].type == sql_token::kLp) {
        ++d;
      } else if (toks[m].type == sql_token::kRp) {
        if (--d == 0) {
          break;
        }
      }
    }
    if (m != e - 1) {
      continue;  // Matching ')' is not the last token -> alias/trailing/complex.
    }
    overrides[i] = ReconstructText(toks, b, e);
  }
  return overrides;
}

// Extracts the column name X from a DuckDB `Referenced column "X" not found ...`
// binder error, or nullopt if the error is not of that shape. DuckDB raises this
// when a double-quoted token is treated as an identifier that does not exist -
// which, in SQLite, would instead be a STRING LITERAL.
std::optional<std::string> ParseReferencedColumnNotFound(
    const std::string& err) {
  static constexpr char kPrefix[] = "Referenced column \"";
  size_t p = err.find(kPrefix);
  if (p == std::string::npos) {
    return std::nullopt;
  }
  p += sizeof(kPrefix) - 1;
  size_t q = err.find("\" not found", p);
  if (q == std::string::npos) {
    return std::nullopt;
  }
  return err.substr(p, q - p);
}

// Rewrites every double-quoted identifier token in `sql` whose UNQUOTED content
// equals `name` into a single-quoted STRING LITERAL, reproducing SQLite's rule
// that a double-quoted token which does not resolve to a column is a string
// literal. The full SQL is reconstructed by concatenating every token (the
// tokenizer emits whitespace and comments as tokens too), so no byte-offset
// bookkeeping is needed. Returns the (possibly unchanged) rewritten SQL.
std::string RewriteDoubleQuotedToString(const std::string& sql,
                                        const std::string& name) {
  std::string out;
  out.reserve(sql.size());
  SqliteTokenizer tokenizer(SqlSource::FromTraceProcessorImplementation(sql));
  for (auto t = tokenizer.Next(); !t.str.empty(); t = tokenizer.Next()) {
    if (t.token_type == sql_token::kId && t.str.size() >= 2 &&
        t.str.front() == '"' && t.str.back() == '"') {
      // Unquote: strip the surrounding quotes, collapse a doubled "" -> ".
      std::string content;
      std::string_view inner = t.str.substr(1, t.str.size() - 2);
      for (size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '"' && i + 1 < inner.size() && inner[i + 1] == '"') {
          content.push_back('"');
          ++i;
        } else {
          content.push_back(inner[i]);
        }
      }
      if (content == name) {
        // Emit as a single-quoted string literal, escaping any single quote.
        out.push_back('\'');
        for (char c : content) {
          if (c == '\'') {
            out.push_back('\'');
          }
          out.push_back(c);
        }
        out.push_back('\'');
        continue;
      }
    }
    out.append(t.str);
  }
  return out;
}

// Extracts the table name X from a DuckDB `Catalog Error: Table with name X does
// not exist!` error, or nullopt otherwise.
std::optional<std::string> ParseTableNotExist(const std::string& err) {
  static constexpr char kPrefix[] = "Table with name ";
  size_t p = err.find(kPrefix);
  if (p == std::string::npos) {
    return std::nullopt;
  }
  p += sizeof(kPrefix) - 1;
  size_t q = err.find(" does not exist", p);
  if (q == std::string::npos) {
    return std::nullopt;
  }
  return err.substr(p, q - p);
}

// Extracts the column name X from a DuckDB `column "X" must appear in the GROUP
// BY clause ...` binder error, or nullopt otherwise. DuckDB raises this for the
// SQLite-lax-aggregate pattern (a projected column neither grouped nor
// aggregated); SQLite picks an arbitrary row's value, which - when the GROUP BY
// key is unique - is the single functionally-determined value.
std::optional<std::string> ParseUngroupedColumn(const std::string& err) {
  static constexpr char kPrefix[] = "column \"";
  size_t p = err.find(kPrefix);
  if (p == std::string::npos) {
    return std::nullopt;
  }
  p += sizeof(kPrefix) - 1;
  size_t q = err.find("\" must appear in the GROUP BY", p);
  if (q == std::string::npos) {
    return std::nullopt;
  }
  return err.substr(p, q - p);
}

// All SQL tokens (including whitespace/comments), for faithful reconstruction.
struct AllToken {
  int type;
  std::string str;
  bool significant;
};
std::vector<AllToken> TokenizeAll(const std::string& sql) {
  std::vector<AllToken> out;
  SqliteTokenizer tokenizer(SqlSource::FromTraceProcessorImplementation(sql));
  for (auto t = tokenizer.Next(); !t.str.empty(); t = tokenizer.Next()) {
    bool sig = t.token_type != sql_token::kSpace &&
               t.token_type != sql_token::kComment;
    out.push_back(AllToken{t.token_type, std::string(t.str), sig});
  }
  return out;
}

// Renames every `format(` function call to `printf(` in `sql`. SQLite's
// `format`/`printf` use C-style `%` specifiers; DuckDB's `printf` is the C-style
// one (DuckDB's `format` is Python `{}`-style), so the rename makes a SQLite
// format() call evaluate identically in DuckDB. SQLite-only specifiers (%q/%Q/
// %w/%z) make DuckDB printf error -> safe fallback. Reconstructs from the full
// token stream so only a function-call `format` (a bare id followed by `(`, not
// a dotted member) is renamed - never a column/string literal named "format".
std::string RewriteFormatToPrintf(const std::string& sql) {
  std::vector<AllToken> toks = TokenizeAll(sql);
  std::string out;
  out.reserve(sql.size() + 8);
  int prev_sig = -1;  // type of previous significant token.
  for (size_t i = 0; i < toks.size(); ++i) {
    const AllToken& t = toks[i];
    bool rename = false;
    if (t.significant && t.type == sql_token::kId &&
        base::ToLower(t.str) == "format" && prev_sig != sql_token::kDot) {
      for (size_t j = i + 1; j < toks.size(); ++j) {
        if (!toks[j].significant) {
          continue;
        }
        rename = toks[j].type == sql_token::kLp;
        break;
      }
    }
    out += rename ? "printf" : t.str;
    if (t.significant) {
      prev_sig = t.type;
    }
  }
  return out;
}

// Rewrites SQLite's single-argument `char(X)` to DuckDB's `chr(CAST(X AS
// INTEGER))` (DuckDB has no `char`, and its `chr` takes a 32-bit INTEGER, not
// the BIGINT that PerfettoSQL columns are). ONLY the single-argument form is
// rewritten: SQLite's `char` is variadic (char(a,b,..) builds a multi-char
// string) whereas `chr` takes one codepoint, so a call with a top-level comma
// is left untouched (it will fall back rather than silently produce a wrong
// result).
std::string RewriteCharToChr(const std::string& sql) {
  std::string cur = sql;
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    int prev_sig = -1;
    size_t match_char = toks.size(), match_lp = toks.size(),
           match_rp = toks.size();
    for (size_t i = 0; i < toks.size() && match_char == toks.size(); ++i) {
      const AllToken& t = toks[i];
      if (!t.significant) {
        continue;
      }
      bool is_char = t.type == sql_token::kId &&
                     base::ToLower(t.str) == "char" &&
                     prev_sig != sql_token::kDot;
      prev_sig = t.type;
      if (!is_char) {
        continue;
      }
      size_t lp = toks.size();
      for (size_t j = i + 1; j < toks.size(); ++j) {
        if (toks[j].significant) {
          lp = (toks[j].type == sql_token::kLp) ? j : toks.size();
          break;
        }
      }
      if (lp == toks.size()) {
        continue;
      }
      int depth = 0;
      bool single_arg = true;
      size_t rp = toks.size();
      for (size_t j = lp; j < toks.size(); ++j) {
        if (!toks[j].significant) {
          continue;
        }
        int tt = toks[j].type;
        if (tt == sql_token::kLp) {
          ++depth;
        } else if (tt == sql_token::kRp) {
          if (--depth == 0) {
            rp = j;
            break;
          }
        } else if (depth == 1 && tt == sql_token::kComma) {
          single_arg = false;
          break;
        }
      }
      if (single_arg && rp != toks.size()) {
        match_char = i;
        match_lp = lp;
        match_rp = rp;
      }
    }
    if (match_char == toks.size()) {
      break;  // No more single-arg char() calls.
    }
    // Arg text is everything between the '(' and the matching ')'.
    std::string arg;
    for (size_t j = match_lp + 1; j < match_rp; ++j) {
      arg += toks[j].str;
    }
    std::string repl = "chr(CAST(" + base::TrimWhitespace(arg) + " AS INTEGER))";
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == match_char) {
        out += repl;
        i = match_rp;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

// Rewrites SQLite's table-valued-function join `JOIN name(args) [AS a]` (no ON)
// into DuckDB's `JOIN name(args) [AS a] ON true`. SQLite allows a table function
// in a JOIN with no ON (an implicit correlated/lateral join); DuckDB requires an
// ON, and treats `ON true` over a correlated table macro as a lateral join
// (preserving LEFT-join semantics). Only a JOIN onto a known table macro
// (`macros`) not already followed by ON/USING is rewritten.
std::string RewriteTableFunctionJoins(
    const std::string& sql,
    const std::unordered_set<std::string>& macros) {
  std::vector<AllToken> toks = TokenizeAll(sql);
  std::vector<size_t> sig;
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].significant) {
      sig.push_back(i);
    }
  }
  auto is_bare_id = [&](size_t s) {
    const AllToken& t = toks[sig[s]];
    return t.type == sql_token::kId && !t.str.empty() && t.str.front() != '"' &&
           t.str.front() != '`';
  };
  std::vector<size_t> insert_after;
  for (size_t k = 0; k + 1 < sig.size(); ++k) {
    if (toks[sig[k]].type != sql_token::kJoin) {
      continue;
    }
    size_t name = k + 1;
    if (!is_bare_id(name) ||
        macros.find(base::ToLower(toks[sig[name]].str)) == macros.end()) {
      continue;
    }
    if (name + 1 >= sig.size() || toks[sig[name + 1]].type != sql_token::kLp) {
      continue;
    }
    int d = 0;
    size_t m = name + 1;
    for (; m < sig.size(); ++m) {
      if (toks[sig[m]].type == sql_token::kLp) {
        ++d;
      } else if (toks[sig[m]].type == sql_token::kRp) {
        if (--d == 0) {
          break;
        }
      }
    }
    if (m >= sig.size()) {
      continue;
    }
    size_t after = m + 1;
    if (after < sig.size() && toks[sig[after]].type == sql_token::kAs) {
      after += 2;
    } else if (after < sig.size() && is_bare_id(after) &&
               toks[sig[after]].type != sql_token::kOn &&
               toks[sig[after]].type != sql_token::kUsing) {
      after += 1;
    }
    if (after < sig.size() && (toks[sig[after]].type == sql_token::kOn ||
                               toks[sig[after]].type == sql_token::kUsing)) {
      continue;
    }
    size_t last_sig = (after == m + 1) ? m : after - 1;
    insert_after.push_back(sig[last_sig]);
  }
  if (insert_after.empty()) {
    return sql;
  }
  std::unordered_set<size_t> ins(insert_after.begin(), insert_after.end());
  std::string out;
  for (size_t i = 0; i < toks.size(); ++i) {
    out += toks[i].str;
    if (ins.find(i) != ins.end()) {
      out += " ON true";
    }
  }
  return out;
}

// True if `name` (lowercased) is an aggregate whose argument is ALREADY a valid
// grouped context - a bare column inside it must NOT be wrapped (that would nest
// aggregates). Covers the SQL standard aggregates plus any_value/group_concat.
bool IsAggregateName(const std::string& lower) {
  static const std::unordered_set<std::string>* kAgg =
      new std::unordered_set<std::string>{
          "count", "sum", "min", "max", "avg", "total",
          "group_concat", "any_value", "string_agg"};
  return kAgg->find(lower) != kAgg->end();
}

// Repairs a DuckDB lax-GROUP-BY rejection by wrapping the FIRST non-aggregated
// occurrence of the bare column reference `[alias.]col` in the top-level
// projection with `ANY_VALUE(...)` - exactly what DuckDB's error suggests, and
// semantically identical to SQLite's "arbitrary row" when the GROUP BY key is
// unique. Works at the EXPRESSION level (so `coalesce(r.x, max(p.x))` wraps only
// the bare `r.x`, never the `max(p.x)` argument), skipping any reference that is
// inside an aggregate call or is a function name / dotted-member prefix. The
// caller retries; DuckDB reports one column at a time. Returns the input
// unchanged if no safe rewrite applies.
std::string RewriteUngroupedColumn(const std::string& sql,
                                   const std::string& col) {
  std::vector<AllToken> toks = TokenizeAll(sql);
  std::string lcol = base::ToLower(col);

  std::vector<size_t> sig;  // all-token indices of significant tokens.
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].significant) {
      sig.push_back(i);
    }
  }
  auto is_bare_id = [&](size_t s) {
    const AllToken& t = toks[sig[s]];
    return t.type == sql_token::kId && !t.str.empty() && t.str.front() != '"' &&
           t.str.front() != '`';
  };

  // First top-level SELECT, then its projection end (top-level FROM/;).
  int depth = 0;
  size_t sel = sig.size();
  for (size_t k = 0; k < sig.size(); ++k) {
    int tt = toks[sig[k]].type;
    if (tt == sql_token::kLp) {
      ++depth;
    } else if (tt == sql_token::kRp) {
      depth = depth > 0 ? depth - 1 : 0;
    } else if (depth == 0 && tt == sql_token::kSelect) {
      sel = k;
      break;
    }
  }
  if (sel == sig.size()) {
    return sql;
  }
  depth = 0;
  size_t proj_end = sig.size();
  for (size_t k = sel + 1; k < sig.size(); ++k) {
    int tt = toks[sig[k]].type;
    if (tt == sql_token::kLp) {
      ++depth;
    } else if (tt == sql_token::kRp) {
      depth = depth > 0 ? depth - 1 : 0;
    } else if (depth == 0 &&
               (tt == sql_token::kFrom || tt == sql_token::kSemi)) {
      proj_end = k;
      break;
    }
  }

  // Walk the projection tracking, per open paren, whether it belongs to an
  // aggregate call. A bare `[alias.]col` is wrappable iff no enclosing paren is
  // an aggregate call.
  std::vector<bool> agg_stack;  // one entry per open '(' in the projection.
  for (size_t k = sel + 1; k < proj_end; ++k) {
    int tt = toks[sig[k]].type;
    if (tt == sql_token::kLp) {
      // Is this the arg-list of an aggregate function? i.e. the previous
      // significant token is a bare id naming an aggregate.
      bool is_agg = k > 0 && is_bare_id(k - 1) &&
                    IsAggregateName(base::ToLower(toks[sig[k - 1]].str));
      agg_stack.push_back(is_agg);
      continue;
    }
    if (tt == sql_token::kRp) {
      if (!agg_stack.empty()) {
        agg_stack.pop_back();
      }
      continue;
    }
    bool inside_agg = false;
    for (bool a : agg_stack) {
      inside_agg = inside_agg || a;
    }
    if (inside_agg || !is_bare_id(k) ||
        base::ToLower(toks[sig[k]].str) != lcol) {
      continue;
    }
    // `col` token matches. Reject if it is a function name (followed by '(') or
    // the prefix of a dotted member (followed by '.').
    if (k + 1 < sig.size() && (toks[sig[k + 1]].type == sql_token::kLp ||
                               toks[sig[k + 1]].type == sql_token::kDot)) {
      continue;
    }
    // Determine the reference span: a preceding `alias .` makes it `alias.col`.
    size_t ref_first_sig = k;
    if (k >= 2 && toks[sig[k - 1]].type == sql_token::kDot && is_bare_id(k - 2)) {
      ref_first_sig = k - 2;
    }
    size_t r_first = sig[ref_first_sig], r_last = sig[k];
    std::string ref_text;
    for (size_t i = r_first; i <= r_last; ++i) {
      ref_text += toks[i].str;
    }
    // If the reference is a STANDALONE top-level projection item (not inside any
    // paren/expression, preceded by SELECT or a comma and followed by a comma /
    // FROM), wrapping it would rename the output column (`any_value(r.ts)` vs
    // `ts`); add `AS col` to preserve the name. Inside an expression the
    // enclosing alias/name is kept, so no alias is added.
    bool prev_ok = ref_first_sig > 0 &&
                   (toks[sig[ref_first_sig - 1]].type == sql_token::kSelect ||
                    toks[sig[ref_first_sig - 1]].type == sql_token::kComma);
    bool next_ok = (k + 1 >= proj_end) ||
                   toks[sig[k + 1]].type == sql_token::kComma ||
                   toks[sig[k + 1]].type == sql_token::kFrom;
    bool standalone = agg_stack.empty() && prev_ok && next_ok;
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == r_first) {
        out += "ANY_VALUE(" + ref_text + ")";
        if (standalone) {
          out += " AS " + col;
        }
        i = r_last;
        continue;
      }
      out += toks[i].str;
    }
    return out;
  }
  return sql;
}

}  // namespace

std::string RewriteIntervalIntersectMacro(const std::string& sql) {
  std::string cur = sql;
  // Rewrite occurrences one at a time, re-tokenizing after each (the rewrite
  // changes offsets). Capped to avoid any pathological loop.
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    std::vector<size_t> sig;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].significant) {
        sig.push_back(i);
      }
    }
    auto is_bare_id = [&](size_t s) {
      const AllToken& t = toks[sig[s]];
      return t.type == sql_token::kId && !t.str.empty() &&
             t.str.front() != '"' && t.str.front() != '`';
    };
    // Find `_interval_intersect ! (`.
    size_t k = sig.size();
    for (size_t j = 0; j + 2 < sig.size(); ++j) {
      if (is_bare_id(j) &&
          base::ToLower(toks[sig[j]].str) == "_interval_intersect" &&
          toks[sig[j + 1]].type == sql_token::kBang &&
          toks[sig[j + 2]].type == sql_token::kLp) {
        k = j;
        break;
      }
    }
    if (k == sig.size()) {
      break;  // No more occurrences.
    }
    // Parse the two parenthesized lists inside the outer '(' at sig[k+2].
    // Outer '(' opens at sig index ko = k+2. Inside: '(' tables ')' ',' '('
    // partitions ')'. Find the outer matching ')'.
    size_t ko = k + 2;
    int depth = 0;
    size_t outer_close = sig.size();
    for (size_t m = ko; m < sig.size(); ++m) {
      if (toks[sig[m]].type == sql_token::kLp) {
        ++depth;
      } else if (toks[sig[m]].type == sql_token::kRp) {
        if (--depth == 0) {
          outer_close = m;
          break;
        }
      }
    }
    if (outer_close == sig.size()) {
      break;  // Malformed; leave as-is (will fall back).
    }
    // Collect bare-id names within the first and second top-level (depth-1)
    // nested paren groups inside the outer parens.
    std::vector<std::string> tables, partitions;
    int group = 0;  // 0 = before first '(', 1 = in tables, 2 = between, 3 = in
                    // partitions.
    int d = 0;
    for (size_t m = ko + 1; m < outer_close; ++m) {
      int tt = toks[sig[m]].type;
      if (tt == sql_token::kLp) {
        ++d;
        if (d == 1) {
          group = (group == 0) ? 1 : 3;
        }
        continue;
      }
      if (tt == sql_token::kRp) {
        --d;
        continue;
      }
      if (d == 1 && is_bare_id(m)) {
        if (group == 1) {
          tables.push_back(toks[sig[m]].str);
        } else if (group == 3) {
          partitions.push_back(toks[sig[m]].str);
        }
      }
    }
    if (tables.empty()) {
      break;  // Nothing sensible to rewrite.
    }
    // Non-partitioned: run the native N-way interval intersector via the
    // __intrinsic_ii_agg aggregate (one opaque handle per input table) +
    // __intrinsic_ii_combine scalar (returns a LIST<STRUCT> of result rows),
    // UNNESTed into rows. This reuses Trace Processor's own algorithm rather
    // than betting on the DuckDB planner not to blow an N-way SQL overlap join
    // into an O(N^k) nested loop. (Partitioned calls still use the SQL IEJoin
    // form below until the combiner learns partitions.)
    std::string sub;
    if (partitions.empty()) {
      sub = "(SELECT ii.u.ts AS ts, ii.u.dur AS dur";
      for (size_t i = 0; i < tables.size(); ++i) {
        std::string c = "id_" + std::to_string(i);
        sub += ", ii.u." + c + " AS " + c;
      }
      sub += " FROM (SELECT unnest(__intrinsic_ii_combine([";
      for (size_t i = 0; i < tables.size(); ++i) {
        sub += (i ? ", " : "") +
               std::string("(SELECT __intrinsic_ii_agg(id, ts, dur) FROM ") +
               tables[i] + ")";
      }
      sub += "])) AS u) ii)";
      size_t first_tok_np = sig[k], last_tok_np = sig[outer_close];
      std::string out_np;
      for (size_t i = 0; i < toks.size(); ++i) {
        if (i == first_tok_np) {
          out_np += sub;
          i = last_tok_np;
          continue;
        }
        out_np += toks[i].str;
      }
      cur = std::move(out_np);
      continue;
    }
    // Build the overlap-join subquery (partitioned path).
    auto starts = [&]() {
      std::string s;
      for (size_t i = 0; i < tables.size(); ++i) {
        s += (i ? ", " : "") + tables[i] + ".ts";
      }
      return s;
    };
    auto ends = [&]() {
      std::string s;
      for (size_t i = 0; i < tables.size(); ++i) {
        s += (i ? ", " : "") + tables[i] + ".ts + " + tables[i] + ".dur";
      }
      return s;
    };
    std::string ts_expr = tables.size() == 1
                              ? tables[0] + ".ts"
                              : "greatest(" + starts() + ")";
    std::string end_expr = tables.size() == 1
                               ? tables[0] + ".ts + " + tables[0] + ".dur"
                               : "least(" + ends() + ")";
    sub = "(SELECT " + ts_expr + " AS ts, (" + end_expr + ") - (" +
          ts_expr + ") AS dur";
    for (size_t i = 0; i < tables.size(); ++i) {
      sub += ", " + tables[i] + ".id AS id_" + std::to_string(i);
    }
    for (const std::string& p : partitions) {
      sub += ", " + tables[0] + "." + p + " AS " + p;
    }
    sub += " FROM ";
    for (size_t i = 0; i < tables.size(); ++i) {
      sub += (i ? ", " : "") + tables[i];
    }
    std::string where;
    // Pairwise interval-overlap predicates. By Helly's theorem in one
    // dimension, a set of (convex) intervals has a common intersection iff
    // every pair overlaps, so the N-way intersection is exactly the set of
    // tuples whose intervals pairwise overlap. Crucially, emitting the pairwise
    // *simple* inequalities `a.ts < cmp_end(b) AND b.ts < cmp_end(a)` lets
    // DuckDB plan an inequality/range join (IEJoin, ~O(N log N)); the single
    // compound `greatest(starts) < least(ends)` predicate instead forces a full
    // O(N^k) nested-loop cartesian product that hangs on large traces.
    //
    // `cmp_end` is `ts + dur` except for an instant (dur == 0), where it is
    // bumped by 1 so the half-open `<` test still admits the instant's single
    // point (an instant behaves as a closed point: it intersects an interval
    // [c, d) iff c <= p < d, and another instant iff equal). The bump affects
    // ONLY the overlap test; the result `dur` below uses the real ends, so a
    // matched instant still yields dur = 0. The bump is conditional on dur == 0
    // so real intervals are unchanged and touching intervals stay excluded.
    auto cmp_end = [&](const std::string& t) {
      return t + ".ts + " + t + ".dur + CASE WHEN " + t + ".dur = 0 THEN 1 ELSE 0 END";
    };
    for (size_t i = 0; i < tables.size(); ++i) {
      for (size_t j = i + 1; j < tables.size(); ++j) {
        if (!where.empty()) {
          where += " AND ";
        }
        where += tables[i] + ".ts < " + cmp_end(tables[j]) + " AND " +
                 tables[j] + ".ts < " + cmp_end(tables[i]);
      }
    }
    for (const std::string& p : partitions) {
      for (size_t i = 1; i < tables.size(); ++i) {
        if (!where.empty()) {
          where += " AND ";
        }
        where += tables[0] + "." + p + " = " + tables[i] + "." + p;
      }
    }
    if (!where.empty()) {
      sub += " WHERE " + where;
    }
    sub += ")";
    // Replace all-token span [sig[k] .. sig[outer_close]] with `sub`.
    size_t first_tok = sig[k], last_tok = sig[outer_close];
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == first_tok) {
        out += sub;
        i = last_tok;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

std::string RewriteGraphReachableMacro(const std::string& sql) {
  std::string cur = sql;
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    std::vector<size_t> sig;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].significant) {
        sig.push_back(i);
      }
    }
    auto is_bare_id = [&](size_t s) {
      const AllToken& t = toks[sig[s]];
      return t.type == sql_token::kId && !t.str.empty() &&
             t.str.front() != '"' && t.str.front() != '`';
    };
    // Find `graph_reachable_bfs ! (` or `graph_reachable_dfs ! (`.
    size_t k = sig.size();
    const char* combiner = nullptr;
    for (size_t j = 0; j + 2 < sig.size(); ++j) {
      if (!is_bare_id(j) || toks[sig[j + 1]].type != sql_token::kBang ||
          toks[sig[j + 2]].type != sql_token::kLp) {
        continue;
      }
      std::string name = base::ToLower(toks[sig[j]].str);
      if (name == "graph_reachable_bfs") {
        combiner = "__intrinsic_graph_bfs";
      } else if (name == "graph_reachable_dfs") {
        combiner = "__intrinsic_graph_dfs";
      } else {
        continue;
      }
      k = j;
      break;
    }
    if (k == sig.size()) {
      break;  // No more occurrences.
    }
    // Parse the two top-level args (graph_table, start_nodes) by text.
    size_t ko = k + 2;
    int depth = 0;
    size_t outer_close = sig.size();
    std::vector<size_t> commas;
    for (size_t m = ko; m < sig.size(); ++m) {
      int tt = toks[sig[m]].type;
      if (tt == sql_token::kLp) {
        ++depth;
      } else if (tt == sql_token::kRp) {
        if (--depth == 0) {
          outer_close = m;
          break;
        }
      } else if (depth == 1 && tt == sql_token::kComma) {
        commas.push_back(m);
      }
    }
    if (outer_close == sig.size() || commas.size() != 1) {
      break;  // Expect exactly two args.
    }
    auto text_between = [&](size_t lo_excl, size_t hi_excl) {
      size_t a = sig[lo_excl] + 1, b = sig[hi_excl];
      std::string s;
      for (size_t i = a; i < b; ++i) {
        s += toks[i].str;
      }
      return base::TrimWhitespace(s);
    };
    std::string graph_arg = text_between(ko, commas[0]);
    std::string start_arg = text_between(commas[0], outer_close);
    if (graph_arg.empty() || start_arg.empty()) {
      break;
    }
    // Aggregate each input into an opaque handle, then run the native BFS/DFS
    // in the combiner; UNNEST the LIST<STRUCT> back into rows. The arg text is
    // used verbatim as a FROM source (a bare table name or a subquery both
    // work), matching the surface macro's TableOrSubquery params.
    std::string sub =
        "(SELECT gr.u.node_id AS node_id, gr.u.parent_node_id AS "
        "parent_node_id FROM (SELECT unnest(" +
        std::string(combiner) +
        "((SELECT __intrinsic_graph_agg(source_node_id, dest_node_id) FROM " +
        graph_arg + "), (SELECT __intrinsic_int_array_agg(node_id) FROM " +
        start_arg + "))) AS u) gr)";
    size_t first_tok = sig[k], last_tok = sig[outer_close];
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == first_tok) {
        out += sub;
        i = last_tok;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

std::string RewriteGraphDominatorMacro(const std::string& sql) {
  std::string cur = sql;
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    std::vector<size_t> sig;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].significant) {
        sig.push_back(i);
      }
    }
    auto is_bare_id = [&](size_t s) {
      const AllToken& t = toks[sig[s]];
      return t.type == sql_token::kId && !t.str.empty() &&
             t.str.front() != '"' && t.str.front() != '`';
    };
    // Find `graph_dominator_tree ! (`.
    size_t k = sig.size();
    for (size_t j = 0; j + 2 < sig.size(); ++j) {
      if (is_bare_id(j) &&
          base::ToLower(toks[sig[j]].str) == "graph_dominator_tree" &&
          toks[sig[j + 1]].type == sql_token::kBang &&
          toks[sig[j + 2]].type == sql_token::kLp) {
        k = j;
        break;
      }
    }
    if (k == sig.size()) {
      break;
    }
    size_t ko = k + 2;
    int depth = 0;
    size_t outer_close = sig.size();
    std::vector<size_t> commas;
    for (size_t m = ko; m < sig.size(); ++m) {
      int tt = toks[sig[m]].type;
      if (tt == sql_token::kLp) {
        ++depth;
      } else if (tt == sql_token::kRp) {
        if (--depth == 0) {
          outer_close = m;
          break;
        }
      } else if (depth == 1 && tt == sql_token::kComma) {
        commas.push_back(m);
      }
    }
    if (outer_close == sig.size() || commas.size() != 1) {
      break;  // Expect exactly two args (graph_table, root_node_id).
    }
    auto text_between = [&](size_t lo_excl, size_t hi_excl) {
      size_t a = sig[lo_excl] + 1, b = sig[hi_excl];
      std::string s;
      for (size_t i = a; i < b; ++i) {
        s += toks[i].str;
      }
      return base::TrimWhitespace(s);
    };
    std::string graph_arg = text_between(ko, commas[0]);
    std::string root_arg = text_between(commas[0], outer_close);
    if (graph_arg.empty() || root_arg.empty()) {
      break;
    }
    // The dominator tree has a single input relation (the root is a scalar
    // arg), so a single aggregate over the graph table returns the result rows
    // directly as a LIST<STRUCT>, which we UNNEST. See RegisterDominatorTree.
    std::string sub =
        "(SELECT dt.u.node_id AS node_id, dt.u.dominator_node_id AS "
        "dominator_node_id FROM (SELECT "
        "unnest(__intrinsic_dominator_tree(source_node_id, dest_node_id, (" +
        root_arg + "))) AS u FROM " + graph_arg + ") dt)";
    size_t first_tok = sig[k], last_tok = sig[outer_close];
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == first_tok) {
        out += sub;
        i = last_tok;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

std::string RewriteIntervalIntersectSingleMacro(const std::string& sql) {
  std::string cur = sql;
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    std::vector<size_t> sig;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].significant) {
        sig.push_back(i);
      }
    }
    auto is_bare_id = [&](size_t s) {
      const AllToken& t = toks[sig[s]];
      return t.type == sql_token::kId && !t.str.empty() &&
             t.str.front() != '"' && t.str.front() != '`';
    };
    size_t k = sig.size();
    for (size_t j = 0; j + 2 < sig.size(); ++j) {
      if (is_bare_id(j) &&
          base::ToLower(toks[sig[j]].str) == "_interval_intersect_single" &&
          toks[sig[j + 1]].type == sql_token::kBang &&
          toks[sig[j + 2]].type == sql_token::kLp) {
        k = j;
        break;
      }
    }
    if (k == sig.size()) {
      break;
    }
    size_t ko = k + 2;
    int depth = 0;
    size_t outer_close = sig.size();
    std::vector<size_t> commas;
    for (size_t m = ko; m < sig.size(); ++m) {
      int tt = toks[sig[m]].type;
      if (tt == sql_token::kLp) {
        ++depth;
      } else if (tt == sql_token::kRp) {
        if (--depth == 0) {
          outer_close = m;
          break;
        }
      } else if (depth == 1 && tt == sql_token::kComma) {
        commas.push_back(m);
      }
    }
    if (outer_close == sig.size() || commas.size() != 2) {
      break;  // Expect three args: ts, dur, table.
    }
    auto text_between = [&](size_t lo_excl, size_t hi_excl) {
      size_t a = sig[lo_excl] + 1, b = sig[hi_excl];
      std::string s;
      for (size_t i = a; i < b; ++i) {
        s += toks[i].str;
      }
      return base::TrimWhitespace(s);
    };
    std::string ts_arg = text_between(ko, commas[0]);
    std::string dur_arg = text_between(commas[0], commas[1]);
    std::string tab = text_between(commas[1], outer_close);
    if (ts_arg.empty() || dur_arg.empty() || tab.empty()) {
      break;
    }
    // Intersect every row of `tab` with the single interval [ts, ts+dur). The
    // result ts/dur use the real ends (greatest/least); the WHERE uses the
    // pairwise overlap test with the instant (dur == 0) bump (see
    // RewriteIntervalIntersectMacro) so a zero-length row or a zero-length
    // single interval still matches as a closed point rather than being
    // dropped by the half-open `<`.
    std::string g = "greatest(t.ts, (" + ts_arg + "))";
    std::string l = "least(t.ts + t.dur, (" + ts_arg + ") + (" + dur_arg + "))";
    std::string t_end = "t.ts + t.dur + CASE WHEN t.dur = 0 THEN 1 ELSE 0 END";
    std::string s_end = "(" + ts_arg + ") + (" + dur_arg +
                        ") + CASE WHEN (" + dur_arg + ") = 0 THEN 1 ELSE 0 END";
    std::string sub = "(SELECT t.id AS id, " + g + " AS ts, (" + l + ") - " + g +
                      " AS dur FROM " + tab + " t WHERE t.ts < " + s_end +
                      " AND (" + ts_arg + ") < " + t_end + ")";
    size_t first_tok = sig[k], last_tok = sig[outer_close];
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == first_tok) {
        out += sub;
        i = last_tok;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

std::string RewriteIntervalCreateMacro(const std::string& sql) {
  std::string cur = sql;
  for (int iter = 0; iter < 64; ++iter) {
    std::vector<AllToken> toks = TokenizeAll(cur);
    std::vector<size_t> sig;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].significant) {
        sig.push_back(i);
      }
    }
    auto is_bare_id = [&](size_t s) {
      const AllToken& t = toks[sig[s]];
      return t.type == sql_token::kId && !t.str.empty() &&
             t.str.front() != '"' && t.str.front() != '`';
    };
    // Find `_interval_create ! (`.
    size_t k = sig.size();
    for (size_t j = 0; j + 2 < sig.size(); ++j) {
      if (is_bare_id(j) && base::ToLower(toks[sig[j]].str) == "_interval_create" &&
          toks[sig[j + 1]].type == sql_token::kBang &&
          toks[sig[j + 2]].type == sql_token::kLp) {
        k = j;
        break;
      }
    }
    if (k == sig.size()) {
      break;
    }
    size_t ko = k + 2;  // the outer '('.
    int depth = 0;
    size_t outer_close = sig.size();
    std::vector<size_t> comma_at;  // top-level (depth-1) comma sig indices.
    for (size_t m = ko; m < sig.size(); ++m) {
      int tt = toks[sig[m]].type;
      if (tt == sql_token::kLp) {
        ++depth;
      } else if (tt == sql_token::kRp) {
        if (--depth == 0) {
          outer_close = m;
          break;
        }
      } else if (depth == 1 && tt == sql_token::kComma) {
        comma_at.push_back(m);
      }
    }
    if (outer_close == sig.size() || comma_at.size() != 1) {
      break;  // Expect exactly two args.
    }
    // arg1 = sig (ko, comma], arg2 = sig (comma, outer_close). Reconstruct text
    // from the original all-tokens between the boundaries.
    auto text_between = [&](size_t sig_lo_excl, size_t sig_hi_excl) {
      // tokens strictly between sig[sig_lo_excl] and sig[sig_hi_excl].
      size_t a = sig[sig_lo_excl] + 1, b = sig[sig_hi_excl];
      std::string s;
      for (size_t i = a; i < b; ++i) {
        s += toks[i].str;
      }
      return s;
    };
    std::string starts = base::TrimWhitespace(text_between(ko, comma_at[0]));
    std::string ends = base::TrimWhitespace(text_between(comma_at[0], outer_close));
    if (starts.empty() || ends.empty()) {
      break;
    }
    // `starts`/`ends` are used directly as table references: a bare table/CTE
    // name (`starts`) or an already-parenthesized subquery (`(SELECT ...)`).
    std::string sub =
        "(SELECT ts, dur FROM (SELECT s.ts AS ts, (SELECT min(e.ts) FROM " +
        ends + " e WHERE e.ts > s.ts) - s.ts AS dur FROM " + starts +
        " s) WHERE dur IS NOT NULL ORDER BY ts)";
    size_t first_tok = sig[k], last_tok = sig[outer_close];
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (i == first_tok) {
        out += sub;
        i = last_tok;
        continue;
      }
      out += toks[i].str;
    }
    cur = std::move(out);
  }
  return cur;
}

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
                           FunctionProvider function_provider,
                           ScalarFunctionProvider scalar_function_provider,
                           TableMaterializer table_materializer)
    : string_pool_(string_pool),
      resolver_(std::move(resolver)),
      view_provider_(std::move(view_provider)),
      function_provider_(std::move(function_provider)),
      scalar_function_provider_(std::move(scalar_function_provider)),
      table_materializer_(std::move(table_materializer)) {}

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

  // Register the polymorphic extract_arg(arg_set_id, key) UDF (UNION return).
  // Its (arg_set_id, key) index is built lazily on first use.
  ASSIGN_OR_RETURN(extract_arg_state_,
                   RegisterExtractArg(conn_, &registered_scalar_functions_));

  // Register the interval_intersect aggregate + combiner (the native N-way
  // algorithm, reached via the _interval_intersect! rewrite in the router).
  RETURN_IF_ERROR(RegisterIntervalIntersect(conn_));

  // Register the graph BFS/DFS aggregates + combiners (native reachability,
  // reached via the graph_reachable_bfs!/_dfs! rewrite in the router).
  RETURN_IF_ERROR(RegisterGraphFunctions(conn_));

  // Register the native dominator-tree aggregate (reached via the
  // graph_dominator_tree! rewrite in the router).
  RETURN_IF_ERROR(RegisterDominatorTree(conn_));

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
    // Create the view, repairing SQLite lax-GROUP-BY rejections by wrapping the
    // offending bare column in ANY_VALUE (see RewriteUngroupedColumn). DuckDB
    // reports one column at a time, so retry (capped).
    std::string vsql = create_view_sql;
    bool created = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
      duckdb_result res;
      if (duckdb_query(conn_, vsql.c_str(), &res) != DuckDBError) {
        duckdb_destroy_result(&res);
        created = true;
        break;
      }
      std::string err = duckdb_result_error(&res);
      duckdb_destroy_result(&res);
      std::optional<std::string> col = ParseUngroupedColumn(err);
      if (!col) {
        break;
      }
      std::string rewritten = RewriteUngroupedColumn(vsql, *col);
      if (rewritten == vsql) {
        break;
      }
      vsql = std::move(rewritten);
    }
    if (!created) {
      continue;  // Left unmirrored; a query referencing it falls back.
    }
    mirrored_views_.insert(lower);
    if (base::ToLower(create_view_sql).find("extract_arg") != std::string::npos) {
      mirrored_uses_extract_arg_ = true;
    }
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
    if (base::ToLower(body).find("extract_arg") != std::string::npos) {
      mirrored_uses_extract_arg_ = true;
    }
  }
}

void DuckDbEngine::SyncScalarFunctions() {
  if (!scalar_function_provider_) {
    return;
  }
  // Mirror each runtime scalar `CREATE PERFETTO FUNCTION` as a DuckDB scalar
  // MACRO so a call `f(args)` binds to DuckDB's own macro. The body is a SELECT
  // (with `$arg` placeholders); wrapped as a scalar subquery `AS (<body>)`.
  // DuckDB binds it EAGERLY at CREATE MACRO time, so a body using SQLite-only
  // dialect, an intrinsic, or a recursive self-reference (DuckDB macros cannot
  // recurse) fails to create and stays unmirrored - a call then errors in DuckDB
  // and falls back. On success the name is added to registered_scalar_functions_
  // so the support predicate treats a call to it as eligible.
  for (const TableFunction& fn : scalar_function_provider_()) {
    std::string lower = base::ToLower(fn.name);
    if (mirrored_scalar_macros_.find(lower) != mirrored_scalar_macros_.end()) {
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
    std::string create = "CREATE OR REPLACE MACRO " + fn.name + "(" + params +
                         ") AS (" + body + ")";
    // DuckDB binds the macro body EAGERLY at CREATE MACRO time, so a body that
    // references a plain SQLite-native table (e.g. trace_start() ->
    // `_trace_bounds`) fails unless that table is in DuckDB's catalog. Retry
    // after materializing such a table (bounded; one missing table per error).
    bool created = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
      duckdb_result res;
      if (duckdb_query(conn_, create.c_str(), &res) != DuckDBError) {
        duckdb_destroy_result(&res);
        created = true;
        break;
      }
      std::string err = duckdb_result_error(&res);
      duckdb_destroy_result(&res);
      std::optional<std::string> tbl =
          table_materializer_ ? ParseTableNotExist(err) : std::nullopt;
      if (!tbl || materialized_tables_.find(*tbl) != materialized_tables_.end() ||
          mirrored_views_.find(base::ToLower(*tbl)) != mirrored_views_.end()) {
        break;
      }
      if (!table_materializer_(*tbl, conn_)) {
        break;
      }
      materialized_tables_.insert(*tbl);
    }
    if (!created) {
      continue;  // Leave unmirrored; a call falls back.
    }
    mirrored_scalar_macros_.insert(lower);
    registered_scalar_functions_.insert(lower);
    if (base::ToLower(body).find("extract_arg") != std::string::npos) {
      mirrored_uses_extract_arg_ = true;
    }
  }
}

void DuckDbEngine::SyncIntrinsicMacros() {
  if (intrinsic_macros_created_) {
    return;
  }
  // DuckDB binds macro bodies eagerly, so `slice` must be mirrored first.
  if (mirrored_views_.find("slice") == mirrored_views_.end()) {
    return;
  }
  // The 13-column output schema of the slice-tree intrinsics (the
  // SliceSubsetTable column list, in order), used by every macro body.
  static constexpr char kCols[] =
      "id, ts, dur, track_id, category, name, depth, parent_id, arg_set_id, "
      "thread_ts, thread_dur, thread_instruction_count, "
      "thread_instruction_delta";
  std::string cols = kCols;
  std::string pcols;  // same columns, prefixed with `p.`.
  {
    pcols = "p.id, p.ts, p.dur, p.track_id, p.category, p.name, p.depth, "
            "p.parent_id, p.arg_set_id, p.thread_ts, p.thread_dur, "
            "p.thread_instruction_count, p.thread_instruction_delta";
  }
  std::string scols;  // same columns, prefixed with `sl.`.
  {
    scols = "sl.id, sl.ts, sl.dur, sl.track_id, sl.category, sl.name, sl.depth, "
            "sl.parent_id, sl.arg_set_id, sl.thread_ts, sl.thread_dur, "
            "sl.thread_instruction_count, sl.thread_instruction_delta";
  }

  // ancestor_slice(start_id): walk slice.parent_id upward from start's parent
  // (mirrors plugins/ancestor GetAncestors). _and_self starts at start itself.
  std::string ancestor =
      "WITH RECURSIVE a AS ("
      "  SELECT " + cols + " FROM __intrinsic_slice WHERE id = "
      "      (SELECT parent_id FROM __intrinsic_slice WHERE id = start_id)"
      "  UNION ALL"
      "  SELECT " + pcols + " FROM __intrinsic_slice p JOIN a ON p.id = a.parent_id) "
      "SELECT " + cols + " FROM a";
  std::string ancestor_self =
      "WITH RECURSIVE a AS ("
      "  SELECT " + cols + " FROM __intrinsic_slice WHERE id = start_id"
      "  UNION ALL"
      "  SELECT " + pcols + " FROM __intrinsic_slice p JOIN a ON p.id = a.parent_id) "
      "SELECT " + cols + " FROM a";

  // descendant_slice(start_id): same-track, deeper slices whose ts is within
  // [start.ts, ts_end] (ts_end = start.ts + start.dur, or +inf for an open
  // slice), keeping a START/END-boundary candidate only if start_id is in its
  // parent chain (the `pclose` recursive parent-closure), exactly mirroring
  // plugins/descendant GetDescendantsInternal + IsAncestor.
  std::string desc_filter =
      " FROM __intrinsic_slice sl, s"
      " WHERE sl.track_id = s.tk AND sl.depth > s.d0"
      "   AND sl.ts >= s.t0 AND sl.ts <= s.te"
      "   AND ((sl.ts > s.t0 AND sl.ts < s.te)"
      "        OR sl.id IN (SELECT id FROM pclose))";
  std::string desc_ctes =
      "WITH RECURSIVE s AS ("
      "  SELECT track_id AS tk, ts AS t0, depth AS d0,"
      "    CASE WHEN dur < 0 THEN 9223372036854775807 ELSE ts + dur END AS te"
      "  FROM __intrinsic_slice WHERE id = start_id),"
      " pclose AS ("
      "  SELECT id FROM __intrinsic_slice WHERE parent_id = start_id"
      "  UNION ALL"
      "  SELECT sl.id FROM __intrinsic_slice sl JOIN pclose ON sl.parent_id = pclose.id)";
  std::string descendant = desc_ctes + " SELECT " + scols + desc_filter;
  std::string descendant_self =
      desc_ctes + " SELECT " + cols + " FROM __intrinsic_slice WHERE id = start_id" +
      " UNION ALL SELECT " + scols + desc_filter;

  struct Macro {
    const char* name;
    const std::string& body;
  };
  const Macro kMacros[] = {
      {"ancestor_slice", ancestor},
      {"_slice_ancestor_and_self", ancestor_self},
      {"descendant_slice", descendant},
      {"_slice_descendant_and_self", descendant_self},
  };
  for (const Macro& m : kMacros) {
    std::string lower = base::ToLower(m.name);
    if (mirrored_table_macros_.find(lower) != mirrored_table_macros_.end()) {
      continue;
    }
    std::string create = std::string("CREATE OR REPLACE MACRO ") + m.name +
                         "(start_id) AS TABLE (" + m.body + ")";
    duckdb_result res;
    if (duckdb_query(conn_, create.c_str(), &res) == DuckDBError) {
      duckdb_destroy_result(&res);
      continue;
    }
    duckdb_destroy_result(&res);
    mirrored_table_macros_.insert(lower);
  }
  intrinsic_macros_created_ = true;
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

  // Re-materialize SQLite-native tables FRESH per query: a materialized table is
  // a SNAPSHOT, and such tables (e.g. a span_join output, or one populated
  // during trace load) may change between queries. DROP the snapshots from the
  // last query and clear the set, so (a) the next reference re-snapshots current
  // data, and (b) a snapshot taken during load - when a real VIEW of the same
  // name had not yet mirrored - no longer SHADOWS that view (DuckDB resolves a
  // catalog table before firing the replacement scan).
  for (const std::string& t : materialized_tables_) {
    duckdb_result drop_res;
    duckdb_query(conn_, ("DROP TABLE IF EXISTS \"" + t + "\"").c_str(),
                 &drop_res);
    duckdb_destroy_result(&drop_res);
  }
  materialized_tables_.clear();

  // Mirror any PerfettoSQL views created since the last query (after_eof
  // prelude, user INCLUDEs, runtime CREATE PERFETTO VIEW) so `FROM <view>`
  // resolves. Cheap: already-mirrored views are skipped.
  SyncViews();

  // Mirror any stdlib RETURNS TABLE functions created since the last query as
  // DuckDB table macros so `FROM name(args)` resolves. Cheap: already-mirrored
  // macros are skipped.
  SyncTableFunctions();

  // Mirror any runtime scalar CREATE PERFETTO FUNCTIONs created since the last
  // query as DuckDB scalar macros so a call `f(args)` resolves. Cheap:
  // already-mirrored macros are skipped.
  SyncScalarFunctions();

  // Create the hardcoded slice-tree intrinsic macros (descendant_slice etc.)
  // once the slice view exists. Cheap: a no-op after the first creation.
  SyncIntrinsicMacros();

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

  // If extract_arg may be called - either the user's query mentions it, or a
  // mirrored view/macro BODY does (the common case: extract_arg lives inside
  // view bodies like counter_track/journald, not the user's SQL) - make sure its
  // (arg_set_id, key) index is built BEFORE execution. The build issues its own
  // DuckDB query, so it must not happen re-entrantly inside the UDF trampoline.
  // Idempotent (built once per engine).
  if (extract_arg_state_ && !extract_arg_state_->built &&
      (mirrored_uses_extract_arg_ ||
       base::ToLower(sql).find("extract_arg") != std::string::npos)) {
    RETURN_IF_ERROR(EnsureExtractArgIndexBuilt(conn_, extract_arg_state_.get()));
  }

  // --- Eligible: execute the whole query inside DuckDB. ---
  // The query is run with a bounded DOUBLE-QUOTE-LITERAL repair loop: DuckDB
  // treats `"x"` strictly as an identifier, but in SQLite a double-quoted token
  // that does not resolve to a column is a STRING LITERAL. So on a
  // `Referenced column "x" not found` binder error we rewrite that `"x"` token
  // to `'x'` and retry, faithfully reproducing SQLite's rule. Each DuckDB error
  // reports one missing column, so the loop converges (capped defensively).
  DuckDbExecutionResult exec;
  exec.last_statement_sql = sql;
  // Rename any SQLite C-style format() call to DuckDB's C-style printf() up
  // front (DuckDB's own `format` is Python-style). Done once, proactively, since
  // `format` is allowlisted on that basis; the column-name override still uses
  // the ORIGINAL sql so an unaliased `format(...)` header matches SQLite.
  std::string run_sql = RewriteFormatToPrintf(sql);
  run_sql = RewriteCharToChr(run_sql);
  // Rewrite SQLite table-valued-function joins (`JOIN tvf(args)` with no ON)
  // into DuckDB's `JOIN tvf(args) ON true` lateral joins, for the slice-tree
  // intrinsic macros and stdlib RETURNS TABLE functions called correlated.
  run_sql = RewriteTableFunctionJoins(run_sql, mirrored_table_macros_);
  constexpr int kMaxQuoteRewrites = 32;
  for (int attempt = 0;; ++attempt) {
    if (duckdb_query(conn_, run_sql.c_str(), &exec.result) != DuckDBError) {
      break;  // Success.
    }
    std::string err = duckdb_result_error(&exec.result);
    duckdb_destroy_result(&exec.result);
    // Try the double-quote-as-string-literal repair before giving up.
    if (attempt < kMaxQuoteRewrites) {
      std::optional<std::string> col = ParseReferencedColumnNotFound(err);
      if (col) {
        std::string rewritten = RewriteDoubleQuotedToString(run_sql, *col);
        if (rewritten != run_sql) {
          run_sql = std::move(rewritten);
          continue;  // Retry with the literal-rewritten SQL.
        }
      }
      // Try the SQLite lax-GROUP-BY repair: wrap the offending bare column in
      // ANY_VALUE (DuckDB reports one column per error, so the loop converges).
      std::optional<std::string> gcol = ParseUngroupedColumn(err);
      if (gcol) {
        std::string rewritten = RewriteUngroupedColumn(run_sql, *gcol);
        if (rewritten != run_sql) {
          run_sql = std::move(rewritten);
          continue;
        }
      }
      // Architectural: a referenced table DuckDB cannot find may be a plain
      // SQLite-native table (e.g. the prelude's `_trace_bounds`), invisible to
      // the dataframe replacement scan. Ask the materializer to copy it into
      // DuckDB's catalog, then retry. Each table is materialized at most once.
      if (table_materializer_) {
        std::optional<std::string> tbl = ParseTableNotExist(err);
        // Never materialize a name that is (or will be) a mirrored VIEW or table
        // MACRO: a snapshot table would shadow it. Those resolve via the
        // replacement scan / catalog once their dependencies exist; a transient
        // "does not exist" for them must NOT be turned into a stale snapshot.
        std::string lower = tbl ? base::ToLower(*tbl) : std::string();
        bool is_view_or_macro =
            tbl && (mirrored_views_.find(lower) != mirrored_views_.end() ||
                    mirrored_table_macros_.find(lower) !=
                        mirrored_table_macros_.end());
        if (tbl && !is_view_or_macro &&
            materialized_tables_.find(*tbl) == materialized_tables_.end()) {
          if (table_materializer_(*tbl, conn_)) {
            materialized_tables_.insert(*tbl);
            continue;  // Retry now that the table exists in DuckDB.
          }
        }
      }
    }
    // Now that the predicate no longer pre-verifies relations (DuckDB's binder
    // is the table oracle), a DuckDB ERROR here can be: a Catalog/Binder error
    // (unknown relation), a Parser error (residual SQLite/PerfettoSQL dialect),
    // or an execution-time error (e.g. a Conversion Error where SQLite's loose
    // typing would have silently coerced). In EVERY one of these cases falling
    // back to SQLite is correctness-SAFE: SQLite then produces the golden
    // result, and if SQLite ALSO errors the diff fails honestly. Crucially,
    // falling back on an ERROR never masks SILENT WRONG OUTPUT - that does not
    // error and is handled by the divergence guards above (order, USING,
    // ceil/floor, function allowlist). So: ANY DuckDB error => ineligible =>
    // fall back (or, with fallback disabled, error honestly so the measurement
    // lane is trustworthy).
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
  // Override DuckDB's canonicalized column headers with SQLite's source-text
  // naming for unaliased bare-function-call projections (e.g. `MAX(id)`,
  // `COUNT(*)`), so the CSV header matches the golden. Only applied when the
  // override count matches the result column count (a safety check against any
  // projection-mapping mismatch); otherwise DuckDB's names are kept verbatim.
  std::vector<std::optional<std::string>> overrides =
      ComputeColumnNameOverrides(sql);
  if (overrides.size() == exec.column_names.size()) {
    for (size_t c = 0; c < overrides.size(); ++c) {
      if (overrides[c]) {
        exec.column_names[c] = *overrides[c];
      }
    }
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
