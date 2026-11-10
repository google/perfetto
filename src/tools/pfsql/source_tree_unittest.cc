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

#include "src/tools/pfsql/source_tree.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::pfsql {
namespace {

const Module& FindModule(const std::vector<Module>& modules,
                         std::string_view name) {
  auto it = std::find_if(modules.begin(), modules.end(),
                         [&](const Module& m) { return m.name == name; });
  PERFETTO_CHECK(it != modules.end());
  return *it;
}

std::vector<std::string> ReferencedNames(const std::vector<Module>& modules,
                                         std::string_view symbol_name) {
  for (const Module& m : modules) {
    for (const Symbol& s : m.symbols) {
      if (s.name == symbol_name)
        return s.references;
    }
  }
  PERFETTO_FATAL("Symbol not found");
}

TEST(SourceTreeTest, ReportsModulesAndReferences) {
  base::TmpDirTree tree;
  tree.AddDir("producer");
  tree.AddFile("producer/tables.sql",
               "CREATE PERFETTO TABLE input_table AS SELECT 1 AS id;");
  tree.AddFile("producer/pf_tables.pf.sql",
               "CREATE PERFETTO TABLE pf_input_table AS SELECT 1 AS id;");
  tree.AddFile("consumer.sql",
               "INCLUDE PERFETTO MODULE producer.tables;\n"
               "INCLUDE PERFETTO MODULE producer.pf_tables;\n"
               "CREATE PERFETTO VIEW output_view AS "
               "SELECT id FROM input_table UNION ALL "
               "SELECT id FROM pf_input_table;");

  base::StatusOr<std::vector<Module>> modules = ParseSourceTrees({tree.path()});

  ASSERT_TRUE(modules.ok()) << modules.status().c_message();
  ASSERT_THAT(*modules, testing::SizeIs(3));
  EXPECT_EQ((*modules)[0].name, "consumer");
  EXPECT_EQ((*modules)[1].name, "producer.pf_tables");
  EXPECT_EQ((*modules)[2].name, "producer.tables");
  EXPECT_THAT((*modules)[0].includes,
              testing::ElementsAre("producer.tables", "producer.pf_tables"));
  EXPECT_THAT(ReferencedNames(*modules, "output_view"),
              testing::ElementsAre("input_table", "pf_input_table"));
}

TEST(SourceTreeTest, ReportsSyntaxErrorsOnce) {
  base::TmpDirTree tree;
  tree.AddFile("uut.sql", "CREATE PERFETTO TABLE t AS SELECT FROM WHERE;");

  base::StatusOr<std::vector<Module>> modules = ParseSourceTrees({tree.path()});

  ASSERT_TRUE(modules.ok()) << modules.status().c_message();
  EXPECT_THAT(FindModule(*modules, "uut").errors, testing::SizeIs(1));
}

class SourceTreeReferencesTest : public testing::Test {
 protected:
  SourceTreeReferencesTest() {
    tree_.AddFile("lib.sql",
                  "CREATE PERFETTO TABLE lib_table AS SELECT 1 AS id;\n"
                  "CREATE PERFETTO TABLE lib_other AS SELECT 1 AS id;\n"
                  "CREATE PERFETTO FUNCTION lib_fn(a LONG) RETURNS LONG AS "
                  "SELECT $a;\n"
                  "CREATE PERFETTO MACRO lib_macro(t TableOrSubquery, e Expr) "
                  "RETURNS TableOrSubquery AS "
                  "(SELECT $e FROM $t JOIN lib_other USING (id));\n");
  }

  std::vector<Module> Parse(const std::string& sql) {
    tree_.AddFile("uut.sql", sql);
    base::StatusOr<std::vector<Module>> modules =
        ParseSourceTrees({tree_.path()});
    PERFETTO_CHECK(modules.ok());
    EXPECT_THAT(FindModule(*modules, "uut").errors, testing::IsEmpty());
    return std::move(*modules);
  }

  base::TmpDirTree tree_;
};

TEST_F(SourceTreeReferencesTest, MacroBodyReferencesBelongToMacro) {
  std::vector<Module> program = Parse(
      "CREATE PERFETTO VIEW uut_view AS "
      "SELECT * FROM lib_macro!(lib_table, id);");

  EXPECT_THAT(ReferencedNames(program, "uut_view"),
              testing::UnorderedElementsAre("lib_macro", "lib_table"));
  EXPECT_THAT(ReferencedNames(program, "lib_macro"),
              testing::UnorderedElementsAre("lib_other"));
}

TEST_F(SourceTreeReferencesTest, CollectsReferencesInMacroBodies) {
  std::vector<Module> program = Parse(
      "CREATE PERFETTO MACRO uut_macro(t TableOrSubquery, e Expr) "
      "RETURNS Expr AS "
      "(SELECT lib_fn($e) FROM $t JOIN lib_table USING (id));");

  EXPECT_THAT(ReferencedNames(program, "uut_macro"),
              testing::UnorderedElementsAre("lib_fn", "lib_table"));
}

TEST_F(SourceTreeReferencesTest, ExprArgumentNamesNoRelation) {
  std::vector<Module> program = Parse(
      "CREATE PERFETTO VIEW uut_view AS "
      "SELECT * FROM lib_macro!((SELECT id FROM lib_other), lib_table);");

  EXPECT_THAT(ReferencedNames(program, "uut_view"),
              testing::UnorderedElementsAre("lib_macro", "lib_other"));
}

TEST_F(SourceTreeReferencesTest, FollowsMacroCallsNestedInArguments) {
  std::vector<Module> program = Parse(
      "CREATE PERFETTO VIEW uut_view AS "
      "SELECT * FROM lib_macro!(lib_macro!(lib_table, id), lib_fn(1));");

  EXPECT_THAT(
      ReferencedNames(program, "uut_view"),
      testing::UnorderedElementsAre("lib_macro", "lib_table", "lib_fn"));
}

TEST_F(SourceTreeReferencesTest, CollectsNonSelectOperands) {
  std::vector<Module> program = Parse(
      "CREATE VIRTUAL TABLE uut_span USING "
      "span_join(lib_table partitioned id, lib_other);\n"
      "CREATE PERFETTO FUNCTION uut_fn(a LONG) RETURNS LONG "
      "DELEGATES TO lib_fn;\n"
      "CREATE PERFETTO TABLE uut_graph AS "
      "GRAPH DOMINATOR TREE NODES lib_table EDGES lib_other FROM lib_table;\n"
      "CREATE PERFETTO INDEX uut_index ON lib_other(id);\n");

  EXPECT_THAT(ReferencedNames(program, "uut_span"),
              testing::UnorderedElementsAre("lib_table", "lib_other"));
  EXPECT_THAT(ReferencedNames(program, "uut_fn"),
              testing::UnorderedElementsAre("lib_fn"));
  EXPECT_THAT(ReferencedNames(program, "uut_graph"),
              testing::UnorderedElementsAre("lib_table", "lib_other"));
  EXPECT_THAT(FindModule(program, "uut").references,
              testing::UnorderedElementsAre("lib_other"));
}

TEST_F(SourceTreeReferencesTest, CollectsFirstNodeOfStatement) {
  std::vector<Module> program =
      Parse("CREATE PERFETTO TABLE uut_table AS FROM lib_table |> WHERE 1;");

  EXPECT_THAT(ReferencedNames(program, "uut_table"),
              testing::UnorderedElementsAre("lib_table"));
}

TEST_F(SourceTreeReferencesTest, CteShadowsSymbol) {
  std::vector<Module> program = Parse(
      "CREATE PERFETTO VIEW uut_view AS "
      "WITH lib_table AS (SELECT 1 AS id) "
      "SELECT * FROM lib_macro!(lib_table, id);");

  EXPECT_THAT(ReferencedNames(program, "uut_view"),
              testing::UnorderedElementsAre("lib_macro"));
}

TEST(SourceTreeTest, ReportsHowTablesWereCreated) {
  base::TmpDirTree tree;
  tree.AddFile("uut.sql",
               "CREATE PERFETTO TABLE p AS SELECT 1 AS x;\n"
               "CREATE TABLE t AS SELECT 1 AS x;\n"
               "CREATE VIEW v AS SELECT 1 AS x;\n"
               "CREATE VIRTUAL TABLE s USING span_join(p, t);\n");

  base::StatusOr<std::vector<Module>> modules = ParseSourceTrees({tree.path()});

  ASSERT_TRUE(modules.ok()) << modules.status().c_message();
  const std::vector<Symbol>& symbols = FindModule(*modules, "uut").symbols;
  ASSERT_THAT(symbols, testing::SizeIs(4));
  EXPECT_EQ(symbols[0].form, SymbolForm::kPerfetto);
  EXPECT_EQ(symbols[1].form, SymbolForm::kPlain);
  EXPECT_EQ(symbols[2].form, SymbolForm::kPlain);
  EXPECT_EQ(symbols[2].kind, SymbolKind::kView);
  EXPECT_EQ(symbols[3].form, SymbolForm::kVirtual);
}

TEST(SourceTreeTest, ReportsMutations) {
  base::TmpDirTree tree;
  tree.AddFile("uut.sql",
               "DROP TABLE IF EXISTS gone;\n"
               "INSERT INTO target VALUES (1);\n");

  base::StatusOr<std::vector<Module>> modules = ParseSourceTrees({tree.path()});

  ASSERT_TRUE(modules.ok()) << modules.status().c_message();
  const std::vector<Mutation>& mutations =
      FindModule(*modules, "uut").mutations;
  ASSERT_THAT(mutations, testing::SizeIs(2));
  EXPECT_EQ(mutations[0].kind, "drop");
  EXPECT_EQ(mutations[0].object_type, "table");
  EXPECT_EQ(mutations[0].name, "gone");
  EXPECT_EQ(mutations[1].kind, "insert");
}

TEST(SourceTreeTest, OnlyTheLikeOperatorCountsAsLike) {
  base::TmpDirTree tree;
  tree.AddFile("no_like.sql",
               "-- Nothing like this.\n"
               "SELECT 'like' AS unlikely FROM t WHERE x GLOB 'a*';\n");
  tree.AddFile("in_macro.sql",
               "CREATE PERFETTO MACRO m(x Expr) RETURNS Expr AS "
               "($x LIKE 'a%');\n");

  base::StatusOr<std::vector<Module>> modules = ParseSourceTrees({tree.path()});

  ASSERT_TRUE(modules.ok()) << modules.status().c_message();
  EXPECT_FALSE(FindModule(*modules, "no_like").uses_like);
  EXPECT_TRUE(FindModule(*modules, "in_macro").uses_like);
}

TEST(SourceTreeTest, ReportsTreeDiscoveryFailure) {
  base::TmpDirTree tree;

  base::StatusOr<std::vector<Module>> modules =
      ParseSourceTrees({tree.path() + "/missing"});

  EXPECT_FALSE(modules.ok());
}

}  // namespace
}  // namespace perfetto::pfsql
