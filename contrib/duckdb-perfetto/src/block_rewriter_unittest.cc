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

#include "contrib/duckdb-perfetto/src/block_rewriter.h"

#include <cctype>
#include <string>
#include <thread>
#include <vector>

#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::duckdb_ext {
namespace {

// The extension tokenizes the outer query with DuckDB's own tokenizer, which
// tests can't link against. For queries without DuckDB-only lexical forms,
// syntaqlite's tokenizer produces the same tokens.
std::vector<OuterToken> TestTokenize(const std::string& s) {
  std::vector<OuterToken> toks;
  SyntaqliteTokenizer* tok = syntaqlite_tokenizer_create_perfetto(nullptr);
  syntaqlite_tokenizer_reset(tok, s.data(),
                             static_cast<SyntaqliteLength>(s.size()));
  SyntaqliteToken t;
  while (syntaqlite_tokenizer_next(tok, &t)) {
    if (t.type == SYNTAQLITE_TK_SPACE || t.type == SYNTAQLITE_TK_COMMENT)
      continue;
    OuterToken::Kind kind = OuterToken::Kind::kOperator;
    char c = t.text[0];
    if (t.type == SYNTAQLITE_TK_STRING ||
        isdigit(static_cast<unsigned char>(c)))
      kind = OuterToken::Kind::kLiteral;
    else if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '"')
      kind = OuterToken::Kind::kWord;
    toks.push_back({static_cast<size_t>(t.text - s.data()), kind});
  }
  syntaqlite_tokenizer_destroy(tok);
  return toks;
}

RewriteResult RewriteForTest(const std::string& q) {
  static BlockRewriter* rewriter = new BlockRewriter();
  return rewriter->Rewrite(q, TestTokenize);
}

std::string Rewrite(const std::string& q) {
  RewriteResult res = RewriteForTest(q);
  EXPECT_EQ(res.status, RewriteResult::kRewritten) << res.error;
  return res.query;
}

TEST(BlockRewriterTest, NoBlocks) {
  EXPECT_EQ(RewriteForTest("SELECT 1").status, RewriteResult::kNoBlocks);
  EXPECT_EQ(RewriteForTest("SELECT perfetto FROM t").status,
            RewriteResult::kNoBlocks);
  // Function-call shape in expression position is not a block.
  EXPECT_EQ(RewriteForTest("SELECT perfetto(x) FROM t").status,
            RewriteResult::kNoBlocks);
  // A quoted identifier is never the PERFETTO keyword.
  EXPECT_EQ(RewriteForTest("FROM \"perfetto\"(t)(SELECT 1)").status,
            RewriteResult::kNoBlocks);
}

TEST(BlockRewriterTest, TableRef) {
  EXPECT_EQ(Rewrite("SELECT * FROM PERFETTO(t) (SELECT 1) AS s"),
            "SELECT * FROM perfetto_query('t', 'SELECT 1') AS s");
  EXPECT_EQ(Rewrite("SELECT * FROM a JOIN perfetto(t)(SELECT 1) b USING(x)"),
            "SELECT * FROM a JOIN perfetto_query('t', 'SELECT 1') b USING(x)");
  EXPECT_EQ(Rewrite("FROM a, PERFETTO(t)(SELECT 1)"),
            "FROM a, perfetto_query('t', 'SELECT 1')");
}

TEST(BlockRewriterTest, StatementAndSubquery) {
  EXPECT_EQ(Rewrite("PERFETTO(t) (INCLUDE PERFETTO MODULE foo)"),
            "FROM perfetto_query('t', 'INCLUDE PERFETTO MODULE foo')");
  EXPECT_EQ(Rewrite("CREATE TABLE x AS PERFETTO(t)(SELECT 1)"),
            "CREATE TABLE x AS FROM perfetto_query('t', 'SELECT 1')");
  EXPECT_EQ(Rewrite("WITH s AS (PERFETTO(t)(SELECT 1)) FROM s"),
            "WITH s AS (FROM perfetto_query('t', 'SELECT 1')) FROM s");
}

TEST(BlockRewriterTest, Unaliased) {
  EXPECT_EQ(Rewrite("FROM PERFETTO (SELECT 1)"),
            "FROM perfetto_query('', 'SELECT 1')");
  EXPECT_EQ(Rewrite("PERFETTO (SELECT (1))"),
            "FROM perfetto_query('', 'SELECT (1)')");
}

TEST(BlockRewriterTest, QuotedAlias) {
  EXPECT_EQ(Rewrite("FROM PERFETTO(\"my \"\"t\"\"\")(SELECT 1)"),
            "FROM perfetto_query('my \"t\"', 'SELECT 1')");
}

TEST(BlockRewriterTest, BodyLexedAsPerfettoSql) {
  // Semicolons, nested parens, quotes and comments inside the block.
  EXPECT_EQ(
      Rewrite("FROM PERFETTO(t)(INCLUDE PERFETTO MODULE m; "
              "SELECT ')' AS x, \"a)\" FROM f((1)) -- )\n/* ) */)"),
      "FROM perfetto_query('t', 'INCLUDE PERFETTO MODULE m; SELECT '')'' AS "
      "x, \"a)\" FROM f((1)) -- )\n/* ) */')");
  // SQLite-only quoting.
  EXPECT_EQ(Rewrite("FROM PERFETTO(t)(SELECT `a)` FROM [b)])"),
            "FROM perfetto_query('t', 'SELECT `a)` FROM [b)]')");
}

TEST(BlockRewriterTest, MacrosAccepted) {
  EXPECT_EQ(Rewrite("FROM PERFETTO(t)(SELECT * FROM my_macro!(slice, (1)))"),
            "FROM perfetto_query('t', 'SELECT * FROM my_macro!(slice, (1))')");
}

TEST(BlockRewriterTest, OuterLiteralsAreOpaque) {
  // Whatever the outer tokenizer reports as a literal (e.g. a DuckDB $$
  // string) is never searched for blocks.
  std::string q = "SELECT $$PERFETTO(t)(x)$$";
  auto tokenize = [](const std::string&) {
    return std::vector<OuterToken>{{0, OuterToken::Kind::kWord},
                                   {7, OuterToken::Kind::kLiteral}};
  };
  BlockRewriter rewriter;
  EXPECT_EQ(rewriter.Rewrite(q, tokenize).status, RewriteResult::kNoBlocks);
}

TEST(BlockRewriterTest, MultipleBlocks) {
  EXPECT_EQ(Rewrite("FROM PERFETTO(a)(SELECT 1) JOIN PERFETTO(b)(SELECT 2) "
                    "USING (x); PERFETTO(a)(SELECT 3)"),
            "FROM perfetto_query('a', 'SELECT 1') JOIN perfetto_query('b', "
            "'SELECT 2') USING (x); FROM perfetto_query('a', 'SELECT 3')");
}

TEST(BlockRewriterTest, ConcurrentRewrites) {
  BlockRewriter rewriter;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&rewriter] {
      for (int j = 0; j < 200; ++j) {
        EXPECT_EQ(
            rewriter.Rewrite("FROM PERFETTO(t)(SELECT 1)", TestTokenize).query,
            "FROM perfetto_query('t', 'SELECT 1')");
        EXPECT_EQ(
            rewriter.Rewrite("FROM PERFETTO(t)(SELECT FROM)", TestTokenize)
                .status,
            RewriteResult::kError);
      }
    });
  }
  for (std::thread& t : threads)
    t.join();
}

TEST(BlockRewriterTest, Unterminated) {
  RewriteResult res = RewriteForTest("FROM PERFETTO(t)(SELECT 1");
  EXPECT_EQ(res.status, RewriteResult::kError);
  EXPECT_EQ(res.error_offset, 5u);
}

TEST(BlockRewriterTest, SyntaxErrorPointsIntoOriginalQuery) {
  //                 0123456789012345678901234567890123456
  std::string q = "FROM PERFETTO(t)(SELECT 1; SELECT FROM x)";
  RewriteResult res = RewriteForTest(q);
  EXPECT_EQ(res.status, RewriteResult::kError);
  EXPECT_EQ(q.substr(res.error_offset, 4), "FROM");
}

}  // namespace
}  // namespace perfetto::duckdb_ext
