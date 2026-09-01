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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
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
  void AddRelation(std::string name, std::vector<std::string> columns) {
    relations_.Insert(std::move(name), TestLeafRelation{std::move(columns)});
  }

  void AddView(std::string name, std::string sql) {
    auto source = std::make_unique<std::string>(std::move(sql));
    ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
    syntaqlite_parser_reset(parser.get(), source->data(),
                            static_cast<uint32_t>(source->size()));
    PERFETTO_CHECK(syntaqlite_parser_next(parser.get()) == SYNTAQLITE_PARSE_OK);
    uint32_t root = syntaqlite_result_root(parser.get());
    relations_.Insert(std::move(name),
                      ViewRelation{std::move(source), std::move(parser), root});
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
    result.columns.reserve(leaf->columns.size());
    for (const std::string& column : leaf->columns) {
      result.columns.push_back(column);
    }
    return result;
  }

  std::optional<ViewDefinition> FindView(std::string_view name) const override {
    const Relation* relation = relations_.Find(std::string(name));
    const auto* view = relation ? std::get_if<ViewRelation>(relation) : nullptr;
    if (!view || !view->source) {
      return std::nullopt;
    }
    return ViewDefinition{{view->parser.get(), view->root}};
  }

 private:
  struct TestLeafRelation {
    std::vector<std::string> columns;
  };
  struct ViewRelation {
    std::unique_ptr<std::string> source;
    ScopedParser parser;
    uint32_t root;
  };
  using Relation = std::variant<TestLeafRelation, ViewRelation>;

  base::FlatHashMap<std::string, Relation> relations_;
};

std::vector<std::string> Show(const RelationLineage& lineage,
                              const Program& program) {
  std::vector<std::string> out;
  for (const ColumnLineage& column : lineage.columns()) {
    std::string value(column.output_name);
    value += "=";
    for (uint32_t i = 0; i < column.origins.size(); ++i) {
      if (i) {
        value += ",";
      }
      value += program.symbol(column.origins[i].relation).name;
      value += ".";
      value += column.origins[i].column_name;
    }
    out.push_back(std::move(value));
  }
  return out;
}

class RelationAnalyzerTest : public ::testing::Test {
 protected:
  RelationAnalyzerTest() : program_(CreateProgram()) {
    catalog_.AddRelation("slice", {"id", "ts", "name"});
    catalog_.AddRelation("thread", {"utid", "name"});
  }

  static Program CreateProgram() {
    ProgramBuilder builder;
    ModuleId module = builder.AddModule("builtin", "");
    builder.AddSymbol(module, "slice", SymbolKind::kTable);
    builder.AddSymbol(module, "thread", SymbolKind::kTable);
    return builder.Build();
  }

  base::StatusOr<RelationLineage> Analyze(const std::string& sql) {
    ScopedParser parser(syntaqlite_parser_create_perfetto(nullptr));
    syntaqlite_parser_reset(parser.get(), sql.data(),
                            static_cast<uint32_t>(sql.size()));
    if (syntaqlite_parser_next(parser.get()) != SYNTAQLITE_PARSE_OK) {
      return base::ErrStatus("could not parse test query");
    }
    RelationAnalyzer analyzer(program_, catalog_);
    return analyzer.AnalyzeQuery(
        {parser.get(), syntaqlite_result_root(parser.get())});
  }

  std::vector<std::string> Select(const std::string& sql) {
    auto result = Analyze(sql);
    EXPECT_TRUE(result.ok()) << sql << ": " << result.status().c_message();
    return result.ok() ? Show(*result, program_) : std::vector<std::string>{};
  }

  Program program_;
  TestCatalog catalog_;
};

TEST_F(RelationAnalyzerTest, ResolvesColumnsAndAliases) {
  EXPECT_THAT(Select("SELECT id AS slice_id, name FROM slice"),
              testing::ElementsAre("slice_id=slice.id", "name=slice.name"));
}

TEST_F(RelationAnalyzerTest, ResultOutlivesParserAndAnalyzer) {
  auto lineage = Analyze("SELECT id AS slice_id FROM slice");
  ASSERT_TRUE(lineage.ok()) << lineage.status().c_message();
  EXPECT_THAT(Show(*lineage, program_),
              testing::ElementsAre("slice_id=slice.id"));
}

TEST_F(RelationAnalyzerTest, OriginsAreRelationSymbols) {
  auto result = Analyze("SELECT id AS harmless_name FROM slice");
  ASSERT_TRUE(result.ok());
  ASSERT_THAT(result->columns().front().origins, testing::SizeIs(1));
  EXPECT_EQ(result->columns().front().origins.front().relation,
            *program_.FindSymbol("slice"));
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

TEST_F(RelationAnalyzerTest, KeepsEveryOriginOfAmbiguousJoinColumn) {
  EXPECT_THAT(Select("SELECT name FROM slice, thread"),
              testing::ElementsAre("name=slice.name,thread.name"));
}

TEST_F(RelationAnalyzerTest, KeepsEveryOriginAcrossCompoundSelect) {
  EXPECT_THAT(Select("SELECT id FROM slice UNION ALL SELECT utid FROM thread"),
              testing::ElementsAre("id=slice.id,thread.utid"));
}

TEST_F(RelationAnalyzerTest, UnknownRelationsAreConservative) {
  EXPECT_THAT(Select("SELECT id FROM unknown_table"),
              testing::ElementsAre("id="));
}

TEST_F(RelationAnalyzerTest, IdentifiesRowOrigin) {
  auto result = Analyze("SELECT id, name FROM slice");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), program_.FindSymbol("slice"));

  result = Analyze("SELECT s.id, t.utid FROM slice s, thread t");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

TEST_F(RelationAnalyzerTest, DetectsRowPreservingViewChains) {
  catalog_.AddView("v1", "CREATE VIEW v1 AS SELECT id, name FROM slice");
  catalog_.AddView("v2", "CREATE VIEW v2 AS SELECT id AS a, name AS b FROM v1");
  RelationAnalyzer analyzer(program_, catalog_);
  auto result = analyzer.AnalyzeRelation("v2");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), program_.FindSymbol("slice"));
}

TEST_F(RelationAnalyzerTest, RejectsFilteredViewAsRowOrigin) {
  catalog_.AddView("v", "CREATE VIEW v AS SELECT id FROM slice WHERE ts > 5");
  RelationAnalyzer analyzer(program_, catalog_);
  auto result = analyzer.AnalyzeRelation("v");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

TEST_F(RelationAnalyzerTest, RejectsOrderedViewAsRowOrigin) {
  catalog_.AddView("v",
                   "CREATE VIEW v AS SELECT id FROM slice ORDER BY ts DESC");
  RelationAnalyzer analyzer(program_, catalog_);
  auto result = analyzer.AnalyzeRelation("v");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_origin(), std::nullopt);
}

}  // namespace
}  // namespace perfetto::perfetto_sql::analysis
