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

#include "src/perfetto_sql/analysis/relation.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::perfetto_sql::analysis {
namespace {

struct ParserDeleter {
  void operator()(SyntaqliteParser* parser) const {
    syntaqlite_parser_destroy(parser);
  }
};
using ScopedParser = std::unique_ptr<SyntaqliteParser, ParserDeleter>;

class TestCatalog : public Catalog {
 public:
  void AddRelation(std::string name,
                   std::vector<std::string> columns,
                   std::vector<std::string> hidden = {}) {
    relations_.Insert(std::move(name),
                      TestLeafRelation{std::move(columns), std::move(hidden)});
  }

  void AddView(std::string name, std::string sql) {
    relations_.Insert(std::move(name), ViewRelation{std::move(sql)});
  }

  std::optional<LeafRelation> FindLeafRelation(
      std::string_view name) const override {
    const Relation* relation = relations_.Find(std::string(name));
    const auto* leaf =
        relation ? std::get_if<TestLeafRelation>(relation) : nullptr;
    if (!leaf) {
      return std::nullopt;
    }
    LeafRelation result;
    result.name = name;
    result.columns.reserve(leaf->columns.size());
    for (const std::string& column : leaf->columns) {
      bool hidden = std::find(leaf->hidden.begin(), leaf->hidden.end(),
                              column) != leaf->hidden.end();
      result.columns.push_back({column, std::nullopt, hidden});
    }
    return result;
  }

  std::optional<std::string> FindViewSql(std::string_view name) const override {
    const Relation* relation = relations_.Find(std::string(name));
    const auto* view = relation ? std::get_if<ViewRelation>(relation) : nullptr;
    return view ? std::make_optional(view->sql) : std::nullopt;
  }

 private:
  struct TestLeafRelation {
    std::vector<std::string> columns;
    std::vector<std::string> hidden;
  };
  struct ViewRelation {
    std::string sql;
  };
  using Relation = std::variant<TestLeafRelation, ViewRelation>;

  base::FlatHashMap<std::string, Relation> relations_;
};

std::vector<std::string> Show(const RelationLineage& lineage) {
  std::vector<std::string> out;
  for (const ColumnLineage& column : lineage.columns()) {
    std::string value(column.output_name);
    value += "=";
    for (uint32_t i = 0; i < column.origins.size(); ++i) {
      if (i) {
        value += ",";
      }
      value += column.origins[i].relation_name;
      value += ".";
      value += column.origins[i].column_name;
    }
    out.push_back(std::move(value));
  }
  return out;
}

class RelationAnalyzerTest : public ::testing::Test {
 protected:
  RelationAnalyzerTest() {
    catalog_.AddRelation("slice", {"id", "ts", "name"});
    catalog_.AddRelation("thread", {"utid", "name"});
  }

  base::StatusOr<RelationLineage> Analyze(const std::string& sql) {
    ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
    syntaqlite_parser_reset(parser.get(), sql.data(),
                            static_cast<uint32_t>(sql.size()));
    if (syntaqlite_parser_next(parser.get()) != SYNTAQLITE_PARSE_OK) {
      return base::ErrStatus("could not parse test query");
    }
    RelationAnalyzer analyzer(catalog_);
    return analyzer.AnalyzeQuery(
        {parser.get(), syntaqlite_result_root(parser.get())});
  }

  std::vector<std::string> Select(const std::string& sql) {
    auto result = Analyze(sql);
    EXPECT_TRUE(result.ok()) << sql << ": " << result.status().c_message();
    return result.ok() ? Show(*result) : std::vector<std::string>{};
  }

  // Analyzes `name`, or with `name` empty the query itself, as read at the
  // select in `sql` whose text is `at`. An error is shown as "!<message>".
  std::vector<std::string> ReadAt(const std::string& sql,
                                  const std::string& at,
                                  const std::string& name) {
    ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
    syntaqlite_parser_set_collect_node_extents(parser.get(), 1);
    syntaqlite_parser_reset(parser.get(), sql.data(),
                            static_cast<uint32_t>(sql.size()));
    if (syntaqlite_parser_next(parser.get()) != SYNTAQLITE_PARSE_OK) {
      return {"!could not parse test query"};
    }
    std::optional<uint32_t> found;
    for (uint32_t id = 0; id < syntaqlite_parser_node_count(parser.get());
         ++id) {
      const auto* node = static_cast<const SyntaqliteNode*>(
          syntaqlite_parser_node(parser.get(), id));
      SyntaqliteLength len = 0;
      SyntaqliteStmtOffset offset = 0;
      const char* text =
          syntaqlite_parser_node_text(parser.get(), id, &len, &offset);
      if (node && node->tag == SYNTAQLITE_NODE_SELECT_STMT && text &&
          std::string_view(text, len) == at) {
        found = id;
      }
    }
    if (!found) {
      return {"!no select '" + at + "'"};
    }
    RelationAnalyzer analyzer(catalog_);
    auto result = name.empty()
                      ? analyzer.AnalyzeQuery({parser.get(), *found})
                      : analyzer.AnalyzeRelation({parser.get(), *found}, name);
    if (!result.ok()) {
      return {"!" + result.status().message()};
    }
    return Show(*result);
  }

  TestCatalog catalog_;
};

TEST_F(RelationAnalyzerTest, ResolvesColumnsAndAliases) {
  EXPECT_THAT(Select("SELECT id AS slice_id, name FROM slice"),
              testing::ElementsAre("slice_id=slice.id", "name=slice.name"));
}

TEST_F(RelationAnalyzerTest, ResultOutlivesParserAndAnalyzer) {
  auto lineage = Analyze("SELECT id AS slice_id FROM slice");
  ASSERT_TRUE(lineage.ok()) << lineage.status().c_message();
  EXPECT_THAT(Show(*lineage), testing::ElementsAre("slice_id=slice.id"));
}

TEST_F(RelationAnalyzerTest, OriginsNameLeafRelations) {
  auto result = Analyze("SELECT id AS harmless_name FROM slice");
  ASSERT_TRUE(result.ok());
  ASSERT_THAT(result->columns().front().origins, testing::SizeIs(1));
  EXPECT_EQ(result->columns().front().origins.front().relation_name, "slice");
}

TEST_F(RelationAnalyzerTest, ExpressionsHaveUnknownOrigin) {
  EXPECT_THAT(Select("SELECT id, ts * 2 AS doubled FROM slice"),
              testing::ElementsAre("id=slice.id", "doubled="));
}

TEST_F(RelationAnalyzerTest, ExpandsStars) {
  EXPECT_THAT(
      Select("SELECT * FROM slice"),
      testing::ElementsAre("id=slice.id", "ts=slice.ts", "name=slice.name"));
}

// As in SQLite, a hidden column is left out of stars but found by name.
TEST_F(RelationAnalyzerTest, StarsSkipHiddenColumns) {
  catalog_.AddRelation("frame", {"ts", "_auto_id"}, {"_auto_id"});
  EXPECT_THAT(Select("SELECT * FROM frame"),
              testing::ElementsAre("ts=frame.ts"));
  EXPECT_THAT(Select("SELECT f.* FROM frame AS f"),
              testing::ElementsAre("ts=frame.ts"));
  EXPECT_THAT(Select("SELECT _auto_id, * FROM frame"),
              testing::ElementsAre("_auto_id=frame._auto_id", "ts=frame.ts"));
}

TEST_F(RelationAnalyzerTest, ExplicitViewColumnNamesReplaceBodyNames) {
  catalog_.AddView("v", "CREATE VIEW v(public_id) AS SELECT id FROM slice");
  EXPECT_THAT(Select("SELECT public_id FROM v"),
              testing::ElementsAre("public_id=slice.id"));
}

TEST_F(RelationAnalyzerTest, UnqualifiedStarCoalescesUsingColumns) {
  EXPECT_THAT(Select("SELECT * FROM slice JOIN thread USING(name)"),
              testing::ElementsAre("id=slice.id", "ts=slice.ts",
                                   "name=slice.name", "utid=thread.utid"));
  EXPECT_THAT(Select("SELECT * FROM slice NATURAL JOIN thread"),
              testing::ElementsAre("id=slice.id", "ts=slice.ts",
                                   "name=slice.name", "utid=thread.utid"));
  EXPECT_THAT(Select("SELECT thread.* FROM slice JOIN thread USING(name)"),
              testing::ElementsAre("utid=thread.utid", "name=thread.name"));
}

TEST_F(RelationAnalyzerTest, FollowsAliasesSubqueriesAndViews) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, name FROM slice");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT id AS renamed FROM v1");
  EXPECT_THAT(Select("SELECT x.renamed FROM (SELECT renamed FROM v2) x"),
              testing::ElementsAre("renamed=slice.id"));
}

// CTEs are in scope for the rest of their WITH clause and its query, hiding
// relations of the same name there, as in SQLite.
TEST_F(RelationAnalyzerTest, Ctes) {
  // Each sees those before it, and is renamed by its list of names.
  EXPECT_THAT(Select(R"(
                WITH
                  a(x) AS (SELECT id FROM slice),
                  b AS (SELECT x AS y, name FROM a, slice)
                SELECT * FROM (SELECT * FROM b)
              )"),
              testing::ElementsAre("y=slice.id", "name=slice.name"));
  // A CTE hides a table of the same name in its query, but not outside the
  // query or in a view the query reads.
  catalog_.AddView("v", "CREATE VIEW v AS SELECT * FROM thread");
  EXPECT_THAT(Select(R"(
                SELECT *
                FROM (
                  WITH thread AS (SELECT id FROM slice)
                  SELECT * FROM thread, v
                ),
                thread
              )"),
              testing::ElementsAre("id=slice.id", "utid=thread.utid",
                                   "name=thread.name", "utid=thread.utid",
                                   "name=thread.name"));
  // Every CTE of a WITH clause is in scope in all of its definitions, so a
  // CTE read before its columns are known, by itself or by an earlier CTE,
  // is never mistaken for a table of the same name.
  EXPECT_THAT(Select(R"(
                WITH
                  a AS (SELECT utid FROM thread),
                  thread AS (SELECT id AS utid FROM slice)
                SELECT * FROM a
              )"),
              testing::ElementsAre("utid="));
  EXPECT_THAT(Select(R"(
                WITH thread AS (SELECT utid FROM thread)
                SELECT * FROM thread
              )"),
              testing::ElementsAre("utid="));
  // A recursive CTE reads itself before its columns are known.
  EXPECT_THAT(Select(R"(
                WITH RECURSIVE n(i) AS (
                  SELECT id FROM slice
                  UNION ALL
                  SELECT i + 1 FROM n WHERE i < 3
                )
                SELECT * FROM n
              )"),
              testing::ElementsAre("i="));
  // A CTE can filter its rows, so what reads one has no row origin.
  auto result =
      Analyze("WITH t AS (SELECT id FROM slice WHERE ts > 5) SELECT id FROM t");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

// What is read at a node of a statement sees the CTEs in scope there, and only
// those.
TEST_F(RelationAnalyzerTest, ReadsSeeTheCtesInScope) {
  const std::string kHere = "SELECT 1 AS here";
  // Through statements, compound selects and joins; the innermost CTE of a
  // name hides the others.
  const std::string kNested = R"(
    CREATE VIEW v AS
    WITH t AS (SELECT utid FROM thread)
    SELECT 1, 2, 3
    UNION ALL
    SELECT * FROM thread JOIN (
      WITH t AS (SELECT id FROM slice)
      SELECT * FROM (SELECT 1 AS here)
    )
  )";
  EXPECT_THAT(ReadAt(kNested, kHere, "t"), testing::ElementsAre("id=slice.id"));
  // A query read there.
  EXPECT_THAT(ReadAt(R"(
                WITH t AS (SELECT id FROM slice)
                SELECT * FROM (SELECT * FROM t)
              )",
                     "SELECT * FROM t", ""),
              testing::ElementsAre("id=slice.id"));
  // Inside a CTE's definition, all the CTEs of its WITH clause, but only those
  // before it are known yet; and none of another subquery's.
  const std::string kDefinition = R"(
    SELECT *
    FROM (WITH s AS (SELECT id FROM slice) SELECT * FROM s),
    (
      WITH
        a AS (SELECT id FROM slice),
        b AS (SELECT * FROM (SELECT 1 AS here)),
        c AS (SELECT utid FROM thread)
      SELECT * FROM b
    )
  )";
  EXPECT_THAT(ReadAt(kDefinition, kHere, "a"),
              testing::ElementsAre("id=slice.id"));
  EXPECT_THAT(
      ReadAt(kDefinition, kHere, "c"),
      testing::ElementsAre("!relation analysis: CTE 'c' has unknown shape"));
  EXPECT_THAT(ReadAt(kDefinition, kHere, "s"),
              testing::ElementsAre("!relation analysis: 's' is not known"));
  // A node where no query can be read has no known scope.
  EXPECT_THAT(
      ReadAt("SELECT * FROM slice WHERE id IN (SELECT 1 AS here)", kHere, "t"),
      testing::ElementsAre("!relation analysis: could not find the scope of "
                           "the node being read"));
}

TEST_F(RelationAnalyzerTest, KeepsEveryOriginOfAmbiguousJoinColumn) {
  EXPECT_THAT(Select("SELECT name FROM slice, thread"),
              testing::ElementsAre("name=slice.name,thread.name"));
}

TEST_F(RelationAnalyzerTest, KeepsEveryOriginAcrossCompoundSelect) {
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT utid FROM thread"),
              testing::ElementsAre("id=slice.id,thread.utid"));
}

TEST_F(RelationAnalyzerTest, UntraceableCompoundArmPoisonsOrigins) {
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT 123"),
              testing::ElementsAre("id="));
  EXPECT_THAT(Select("SELECT 123 AS id UNION ALL SELECT id FROM slice"),
              testing::ElementsAre("id="));
}

TEST_F(RelationAnalyzerTest, UnknownRelationsAreConservative) {
  EXPECT_THAT(Select("SELECT id FROM unknown_table"),
              testing::ElementsAre("id="));
}

TEST_F(RelationAnalyzerTest, IdentifiesRowOrigin) {
  auto result = Analyze("SELECT id, name FROM slice");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(),
            std::make_optional<std::string_view>("slice"));

  result = Analyze("SELECT s.id, t.utid FROM slice s, thread t");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

TEST_F(RelationAnalyzerTest, DetectsRowPreservingViewChains) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, name FROM slice");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT id AS a, name AS b FROM v1");
  auto result = Analyze("SELECT * FROM v2");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(),
            std::make_optional<std::string_view>("slice"));
}

TEST_F(RelationAnalyzerTest, RejectsFilteredViewAsRowOrigin) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id FROM slice WHERE ts > 5");
  auto result = Analyze("SELECT * FROM v");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

TEST_F(RelationAnalyzerTest, RejectsOrderedViewAsRowOrigin) {
  catalog_.AddView("v",
                   "CREATE VIEW v AS SELECT id FROM slice ORDER BY ts DESC");
  auto result = Analyze("SELECT * FROM v");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

}  // namespace
}  // namespace perfetto::perfetto_sql::analysis
