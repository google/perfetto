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

#include "contrib/duckdb-perfetto/src/chain_rewriter.h"

#include <string>

#include "contrib/duckdb-perfetto/src/block_rewriter.h"
#include "contrib/duckdb-perfetto/src/test_tokenizer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::duckdb_ext {
namespace {

RewriteResult RewriteForTest(const std::string& q) {
  static BlockRewriter* blocks = new BlockRewriter();
  return RewriteQuery(blocks, q, TestTokenize);
}

std::string Rewrite(const std::string& q) {
  RewriteResult res = RewriteForTest(q);
  EXPECT_EQ(res.status, RewriteResult::kRewritten) << res.error;
  return res.query;
}

// Expects an error with |msg| pointing at |at| in |q|.
void ExpectError(const std::string& q,
                 const std::string& at,
                 const std::string& msg) {
  RewriteResult res = RewriteForTest(q);
  ASSERT_EQ(res.status, RewriteResult::kError) << res.query;
  EXPECT_EQ(q.substr(res.error_offset, at.size()), at) << res.error;
  EXPECT_THAT(res.error, testing::HasSubstr(msg));
}

TEST(ChainRewriterTest, NoPipesIsUntouched) {
  EXPECT_EQ(RewriteForTest("SELECT a | b, '|>' FROM t -- |>\n/* |> */").status,
            RewriteResult::kNoBlocks);
  // Blocks are rewritten as before; pipes inside them are PerfettoSQL.
  EXPECT_EQ(Rewrite("FROM PERFETTO(t)(FROM slice |> SELECT name)"),
            "FROM perfetto_query('t', 'FROM slice |> SELECT name')");
}

TEST(ChainRewriterTest, Stages) {
  EXPECT_EQ(Rewrite("FROM t |> WHERE a > 1"), "SELECT * FROM t WHERE a > 1");
  EXPECT_EQ(Rewrite("FROM t |> SELECT a, b + 1 AS c"),
            "SELECT a, b + 1 AS c FROM t");
  EXPECT_EQ(Rewrite("FROM t |> EXTEND a + 1 AS c"),
            "SELECT *, a + 1 AS c FROM t");
  EXPECT_EQ(Rewrite("FROM t |> SET a = a * 2, b = f(1, 2)"),
            "SELECT * REPLACE (a * 2 AS a, f(1, 2) AS b) FROM t");
  EXPECT_EQ(Rewrite("FROM t |> DROP a, b"), "SELECT * EXCLUDE (a, b) FROM t");
  EXPECT_EQ(Rewrite("FROM t |> RENAME a AS x, b AS y"),
            "SELECT * RENAME (a AS x, b AS y) FROM t");
  EXPECT_EQ(Rewrite("FROM t |> ORDER BY a DESC, b"),
            "SELECT * FROM t ORDER BY a DESC, b");
  EXPECT_EQ(Rewrite("FROM t |> LIMIT 10 OFFSET 2"),
            "SELECT * FROM t LIMIT 10 OFFSET 2");
}

TEST(ChainRewriterTest, Aggregate) {
  EXPECT_EQ(Rewrite("FROM t |> AGGREGATE count(*) AS n"),
            "SELECT count(*) AS n FROM t");
  EXPECT_EQ(Rewrite("FROM t |> AGGREGATE count(*) AS n, sum(d) AS s "
                    "GROUP BY a, coalesce(b, 0) AS c"),
            "SELECT a, coalesce(b, 0) AS c, count(*) AS n, sum(d) AS s "
            "FROM t GROUP BY 1, 2");
}

TEST(ChainRewriterTest, Join) {
  EXPECT_EQ(Rewrite("FROM t |> JOIN u USING (k)"),
            "SELECT * FROM t JOIN u USING (k)");
  EXPECT_EQ(Rewrite("FROM t AS a |> LEFT JOIN u ON a.k = u.k"),
            "SELECT * FROM t AS a LEFT JOIN u ON a.k = u.k");
}

TEST(ChainRewriterTest, StagesNestAndKeepTheAlias) {
  EXPECT_EQ(Rewrite("FROM t AS s |> WHERE s.a > 1 |> SELECT a |> ORDER BY a"),
            "SELECT * FROM (SELECT a FROM (SELECT * FROM t AS s WHERE s.a > "
            "1) AS s) AS s ORDER BY a");
  EXPECT_EQ(Rewrite("FROM t |> WHERE a |> AS x |> JOIN u ON x.k = u.k"),
            "SELECT * FROM (SELECT * FROM t WHERE a) AS x JOIN u ON x.k = u.k");
  EXPECT_EQ(Rewrite("FROM t |> AS x"), "SELECT * FROM t AS x");
}

TEST(ChainRewriterTest, Sources) {
  EXPECT_EQ(Rewrite("FROM db.main.t |> LIMIT 1"),
            "SELECT * FROM db.main.t LIMIT 1");
  EXPECT_EQ(Rewrite("FROM (SELECT 1 AS a) |> WHERE a = 1"),
            "SELECT * FROM (SELECT 1 AS a) WHERE a = 1");
  EXPECT_EQ(Rewrite("FROM read_csv('x.csv') AS c |> LIMIT 1"),
            "SELECT * FROM read_csv('x.csv') AS c LIMIT 1");
}

TEST(ChainRewriterTest, Blocks) {
  EXPECT_EQ(Rewrite("FROM PERFETTO(t)(SELECT 1 AS a) AS s |> WHERE s.a = 1"),
            "SELECT * FROM perfetto_query('t', 'SELECT 1 AS a') AS s WHERE "
            "s.a = 1");
  // A block on its own is a query too.
  EXPECT_EQ(Rewrite("PERFETTO(t)(SELECT 1 AS a) |> WHERE a = 1"),
            "SELECT * FROM perfetto_query('t', 'SELECT 1 AS a') WHERE a = 1");
  // Pipes inside the block stay in TP; pipes after it run in DuckDB.
  EXPECT_EQ(Rewrite("FROM PERFETTO(t)(PERFETTO PRAGMA pipelines = 1;\n"
                    "  FROM slice |> SELECT name, dur) |> WHERE dur > 0"),
            "SELECT * FROM perfetto_query('t', 'PERFETTO PRAGMA pipelines = "
            "1;\n  FROM slice |> SELECT name, dur') WHERE dur > 0");
}

TEST(ChainRewriterTest, PipelinesNest) {
  EXPECT_EQ(Rewrite("FROM (FROM t |> WHERE a = 1) |> SELECT a"),
            "SELECT a FROM (SELECT * FROM t WHERE a = 1)");
  EXPECT_EQ(Rewrite("SELECT * FROM u WHERE k IN (FROM t |> SELECT k)"),
            "SELECT * FROM u WHERE k IN (SELECT k FROM t)");
  EXPECT_EQ(Rewrite("FROM t |> JOIN (FROM u |> WHERE b) USING (k)"),
            "SELECT * FROM t JOIN (SELECT * FROM u WHERE b) USING (k)");
  EXPECT_EQ(Rewrite("WITH c AS (FROM t |> WHERE a) FROM c"),
            "WITH c AS (SELECT * FROM t WHERE a) FROM c");
  EXPECT_EQ(Rewrite("CREATE TABLE x AS FROM t |> LIMIT 1"),
            "CREATE TABLE x AS SELECT * FROM t LIMIT 1");
  EXPECT_EQ(Rewrite("EXPLAIN ANALYZE FROM t |> LIMIT 1"),
            "EXPLAIN ANALYZE SELECT * FROM t LIMIT 1");
  EXPECT_EQ(Rewrite("FROM t |> LIMIT 1; SELECT 2; FROM u |> LIMIT 3"),
            "SELECT * FROM t LIMIT 1; SELECT 2; SELECT * FROM u LIMIT 3");
}

TEST(ChainRewriterTest, StringsAndComments) {
  EXPECT_EQ(Rewrite("FROM t |> WHERE s = '|>' /* |> */ -- |>\n|> LIMIT 1"),
            "SELECT * FROM (SELECT * FROM t WHERE s = '|>' /* |> */ -- |>\n) "
            "LIMIT 1");
}

TEST(ChainRewriterTest, Errors) {
  ExpectError("SELECT * FROM t |> WHERE a", "|>",
              "|> must follow a pipeline source");
  ExpectError("FROM t WHERE a |> LIMIT 1", "t WHERE",
              "a pipeline must start from a table");
  ExpectError("FROM t |> FROB x", "FROB", "unknown pipe stage");
  ExpectError("FROM t |> INTERVAL FLATTEN AGGREGATE COUNT(*) AS n", "INTERVAL",
              "only available inside PERFETTO blocks");
  ExpectError("FROM t |> |> LIMIT 1", "|> |>", "expected a pipe stage");
  ExpectError("FROM t |> WHERE", "WHERE", "empty WHERE stage");
  ExpectError("FROM t |> SET a", "a", "expected <column> = <expression>");
  ExpectError("FROM t |> AS a b", "AS", "expected a single name after AS");
  ExpectError("FROM t |> AGGREGATE GROUP BY a", "AGGREGATE",
              "expected aggregates");
  ExpectError("SELECT (FROM t |> FROB)", "FROB", "unknown pipe stage");
}

TEST(ChainRewriterTest, ErrorsPointIntoTheOriginalQuery) {
  // After a block, offsets are mapped back through the block's rewrite.
  ExpectError("FROM PERFETTO(t)(SELECT 1) |> FROB", "FROB",
              "unknown pipe stage");
  ExpectError("FROM PERFETTO(t)(SELECT 1) |> WHERE 1 |> FROB", "FROB",
              "unknown pipe stage");
  // An error at the block itself points at the block.
  ExpectError("FROM PERFETTO(t)(SELECT 1) x y |> LIMIT 1", "PERFETTO(t)",
              "a pipeline must start from a table");
  ExpectError("SELECT 1 FROM PERFETTO(t)(SELECT 1) |> LIMIT 1", "|>",
              "|> must follow a pipeline source");
}

TEST(FindFinalSelectTest, Select) {
  auto s = FindFinalSelect("SELECT 1");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->prefix, "");
  EXPECT_EQ(s->select, "SELECT 1");

  s = FindFinalSelect("INCLUDE PERFETTO MODULE m;\nSELECT a FROM b; ");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->prefix, "INCLUDE PERFETTO MODULE m;\n");
  EXPECT_EQ(s->select, "SELECT a FROM b");

  s = FindFinalSelect("WITH x AS (SELECT 1) SELECT * FROM x UNION SELECT 2");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->select, "WITH x AS (SELECT 1) SELECT * FROM x UNION SELECT 2");
}

TEST(FindFinalSelectTest, NotSelect) {
  EXPECT_FALSE(FindFinalSelect("FROM slice |> SELECT name"));
  EXPECT_FALSE(FindFinalSelect("SELECT 1; INCLUDE PERFETTO MODULE m"));
  EXPECT_FALSE(FindFinalSelect("SELECT FROM"));
  EXPECT_FALSE(FindFinalSelect(""));
}

}  // namespace
}  // namespace perfetto::duckdb_ext
