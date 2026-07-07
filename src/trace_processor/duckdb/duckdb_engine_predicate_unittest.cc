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

// Unit tests for the DuckDB support predicate's TOKENIZATION + eligibility
// decision, now driven off the real syntaqlite tokenizer (SqliteTokenizer)
// instead of a hand-rolled character scanner. These exercise the cases that the
// old scanner historically mis-classified - CAST(x AS INT), USING(col), a
// `WITH d(a,b) AS (...)` CTE column list, double-quoted string literals, and a
// real custom-function call - proving the token-driven predicate classifies
// them correctly. No live DuckDB is needed: AnalyzeSupportForTesting is a pure
// function of (SQL, eligible-function-names).

#include <optional>
#include <string>
#include <unordered_set>

#include "src/trace_processor/duckdb/duckdb_engine.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Mirrors the engine's beachhead allowlist closely enough for these tests; the
// real set is internal, but the predicate logic is independent of WHICH names
// are eligible (it only consults membership).
const std::unordered_set<std::string>& Allow() {
  static const std::unordered_set<std::string>* kAllow =
      new std::unordered_set<std::string>{"count", "sum", "min",  "max",
                                          "avg",   "abs", "round"};
  return *kAllow;
}

const std::unordered_set<std::string>& Udfs() {
  static const std::unordered_set<std::string>* kUdfs =
      new std::unordered_set<std::string>{"sqrt", "ln"};
  return *kUdfs;
}

std::optional<std::string> Analyze(const std::string& sql) {
  return internal::AnalyzeSupportForTesting(sql, Allow(), Udfs());
}

bool Eligible(const std::string& sql) {
  return !Analyze(sql).has_value();
}

// --- CAST: a keyword, NOT a function. The old scanner saw `cast(` as a
// function call and had to special-case the keyword; with real tokens CAST is
// its own token type and never reaches the allowlist check. ---
TEST(DuckDbSupportPredicateTest, CastIsNotAFunction) {
  // `CAST(x AS INT)` must not be reported as an unknown function; an ORDER BY
  // keeps the row-order guard happy so eligibility hinges on the CAST handling.
  EXPECT_TRUE(Eligible("SELECT cast(dur AS INT) AS d FROM slice ORDER BY d"));
}

// --- USING: a join clause, NOT a function. The old scanner mis-reported
// `using(` as a missing function; the USING guard was later RELAXED ("list,
// don't guard") because modern DuckDB follows the SQL standard for JOIN ...
// USING. A USING query is now ELIGIBLE (and crucially still not mis-read as a
// missing function). ---
TEST(DuckDbSupportPredicateTest, UsingIsEligibleNotMissingFunction) {
  auto reason = Analyze("SELECT a.x FROM a JOIN b USING(col) ORDER BY a.x");
  ASSERT_FALSE(reason.has_value())
      << "USING should be eligible after the guard relaxation: " << *reason;
}

// --- WITH name(cols) AS (...): the parens hold COLUMN NAMES, not function
// args. The CTE name must NOT be checked against the function allowlist. ---
TEST(DuckDbSupportPredicateTest, CteColumnListIsNotAFunction) {
  EXPECT_TRUE(
      Eligible("WITH d(a, b) AS (SELECT id, dur FROM slice) "
               "SELECT a FROM d ORDER BY a"));
}

// A CTE name that happens to collide with a real function-call site elsewhere
// still must not be treated as a function in the WITH header.
TEST(DuckDbSupportPredicateTest, CteColumnListMultipleColumns) {
  EXPECT_TRUE(
      Eligible("WITH agg(total, mn) AS (SELECT sum(dur), min(ts) FROM slice) "
               "SELECT total FROM agg ORDER BY total"));
}

// --- Double-quoted token: SQLite string literal vs DuckDB quoted identifier.
// The double-quote guard was RELAXED ("list, don't guard"). A double-quoted
// column reference now binds in DuckDB exactly as in SQLite (eligible); a
// double-quoted STRING LITERAL that DuckDB cannot resolve as a column is
// repaired at execution time (the `Referenced column "x" not found` -> rewrite
// `"x"` to `'x'` retry loop). Both cases are eligible at the predicate layer
// and crucially NOT mis-read as an identifier in relation/function position.
// ---
TEST(DuckDbSupportPredicateTest, DoubleQuotedLiteralIsEligible) {
  EXPECT_TRUE(Eligible("SELECT ln(\"as\") FROM slice ORDER BY 1"));
}

TEST(DuckDbSupportPredicateTest, DoubleQuotedIdentifierIsEligible) {
  EXPECT_TRUE(Eligible("SELECT \"dur\" FROM slice ORDER BY 1"));
}

// A single-quoted string literal is a normal STRING token and must NOT be
// confused with a function/relation or trip the double-quote guard.
TEST(DuckDbSupportPredicateTest, SingleQuotedStringIsFine) {
  auto reason =
      Analyze("SELECT name FROM slice WHERE name = 'cast(' ORDER BY 1");
  EXPECT_FALSE(reason.has_value()) << reason.value_or("");
}

// --- A real custom-function call (not in the allowlist) must fall back so an
// unported function is never silently mis-executed. ---
TEST(DuckDbSupportPredicateTest, UnportedFunctionFallsBack) {
  auto reason = Analyze("SELECT my_custom_fn(dur) AS d FROM slice ORDER BY d");
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("my_custom_fn"), std::string::npos) << *reason;
  EXPECT_NE(reason->find("not in allowlist"), std::string::npos) << *reason;
}

// An allowlisted aggregate (with an alias + ORDER BY) is eligible.
TEST(DuckDbSupportPredicateTest, AllowlistedAggregateEligible) {
  EXPECT_TRUE(Eligible("SELECT sum(dur) AS s FROM slice"));
}

// A registered scalar UDF is eligible.
TEST(DuckDbSupportPredicateTest, RegisteredUdfEligible) {
  EXPECT_TRUE(Eligible("SELECT sqrt(dur) AS d FROM slice ORDER BY d"));
}

// A `FROM name(args)` call to a function NOT in any eligibility set is rejected
// (default-deny) - it is not in the allowlist, not a UDF, not a mirrored macro.
TEST(DuckDbSupportPredicateTest, UnmirroredTableFunctionFallsBack) {
  auto reason = internal::AnalyzeSupportForTesting(
      "SELECT * FROM my_table_fn(1, 2)", Allow(), Udfs(), /*table_macros=*/{});
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("my_table_fn"), std::string::npos) << *reason;
}

// A `FROM name(args)` call to a MIRRORED table macro is eligible (treated like
// a table-valued relation: DuckDB binds the macro body). This is the mechanism
// the stdlib RETURNS TABLE -> DuckDB table-macro mirroring relies on.
TEST(DuckDbSupportPredicateTest, MirroredTableMacroEligible) {
  std::unordered_set<std::string> macros{"my_table_fn"};
  EXPECT_FALSE(internal::AnalyzeSupportForTesting(
                   "SELECT * FROM my_table_fn(1, 2)", Allow(), Udfs(), macros)
                   .has_value());
  // It also satisfies the bare-SELECT relation gate even with no other
  // relation.
  EXPECT_FALSE(internal::AnalyzeSupportForTesting(
                   "SELECT x FROM my_table_fn(1) AS t", Allow(), Udfs(), macros)
                   .has_value());
}

// A dotted member `t.count` is a column reference, not a function call, even
// though `count` is an allowlisted name and is (in this contrived case)
// followed by `(` belonging to a different expression.
TEST(DuckDbSupportPredicateTest, DottedMemberIsNotAFunction) {
  // `s.dur` is a column; ensure the relation/projection still classifies.
  EXPECT_TRUE(Eligible("SELECT s.dur AS d FROM slice AS s ORDER BY d"));
}

// --- Macro call `name!(...)`: the dedicated macro pre-check was DROPPED.
// DuckDB cannot parse `!` so it raises a Parser error and the ANY-DuckDB-error
// rule falls back; the predicate no longer needs to detect it statically. The
// predicate therefore reports it ELIGIBLE (DuckDB will then reject it). ---
TEST(DuckDbSupportPredicateTest, MacroCallNoLongerStaticallyGuarded) {
  // `foo!(dur)` is not a function call (foo is followed by `!`, not `(`); with
  // the row-order and macro guards relaxed it is statically eligible - DuckDB's
  // parser is the one that rejects the `!`.
  EXPECT_TRUE(Eligible("SELECT foo!(dur) FROM slice ORDER BY 1"));
}

// A `!=` operator must NOT be mistaken for anything that blocks eligibility.
TEST(DuckDbSupportPredicateTest, NotEqualsIsFine) {
  auto reason = Analyze("SELECT dur FROM slice WHERE dur != 0 ORDER BY 1");
  EXPECT_FALSE(reason.has_value()) << reason.value_or("");
}

// --- Row-order guard RELAXED ("list, don't guard"). A relation scan with no
// top-level ORDER BY (and LIMIT-without-ORDER-BY) is now ELIGIBLE: DuckDB runs
// it and the deterministically-matching results PASS; the genuinely divergent
// tie-break/arbitrary-order cases go to the known-bad list, not a guard. ---
TEST(DuckDbSupportPredicateTest, LimitWithoutOrderByNowEligible) {
  // No longer guarded: let DuckDB run it. (`dur` aliased implicitly so the
  // column-name guard does not bite; it is a column ref, not a function call.)
  EXPECT_TRUE(Eligible("SELECT dur FROM slice LIMIT 10"));
}

TEST(DuckDbSupportPredicateTest, MultiRowScanWithoutOrderByNowEligible) {
  EXPECT_TRUE(Eligible("SELECT dur FROM slice"));
}

TEST(DuckDbSupportPredicateTest, SingleRowAggregateNoOrderByEligible) {
  // A pure aggregate with no GROUP BY yields one row: still eligible.
  EXPECT_TRUE(Eligible("SELECT count(*) AS c FROM slice"));
}

// A scan whose only ordering is inside a subquery is now eligible too (the
// outer row order is decided by DuckDB; matching ones pass, divergent ones
// known-bad).
TEST(DuckDbSupportPredicateTest, OrderByInSubqueryNowEligible) {
  EXPECT_TRUE(Eligible("SELECT dur FROM (SELECT dur FROM slice ORDER BY dur)"));
}

// --- Column-name divergence: an unaliased function-call projection. The GUARD
// was replaced by a post-execution column-name OVERRIDE (DuckDB's
// `count_star()` header is rewritten to the SQLite source text `count(*)`), so
// the query is now ELIGIBLE rather than rejected. ---
TEST(DuckDbSupportPredicateTest, UnaliasedAggregateColumnEligible) {
  EXPECT_TRUE(Eligible("SELECT count(*) FROM slice"));
}

TEST(DuckDbSupportPredicateTest, AliasedAggregateColumnEligible) {
  EXPECT_TRUE(Eligible("SELECT count(*) AS c FROM slice"));
}

TEST(DuckDbSupportPredicateTest, ImplicitlyAliasedAggregateColumnEligible) {
  EXPECT_TRUE(Eligible("SELECT count(*) c FROM slice"));
}

// --- Statement-count + bare-SELECT gates. ---
TEST(DuckDbSupportPredicateTest, MultipleStatementsGuarded) {
  auto reason = Analyze("SELECT 1; SELECT 2");
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("more than one statement"), std::string::npos)
      << *reason;
}

TEST(DuckDbSupportPredicateTest, TrailingSemicolonOk) {
  EXPECT_TRUE(Eligible("SELECT count(*) AS c FROM slice;"));
}

TEST(DuckDbSupportPredicateTest, BareSelectConstantEligible) {
  // The bare-SELECT gate was RELAXED: a no-FROM constant/expression SELECT
  // (e.g. a macro-expanded `SELECT (SELECT 123 - 100)`) evaluates identically
  // in DuckDB and SQLite, so it is now eligible.
  EXPECT_TRUE(Eligible("SELECT 1"));
}

TEST(DuckDbSupportPredicateTest, BareSelectAllowlistedFunctionEligible) {
  // No FROM, but a registered/allowlisted function => routable.
  EXPECT_TRUE(Eligible("SELECT sqrt(2) AS x"));
}

}  // namespace
}  // namespace perfetto::trace_processor::duckdb_integration
