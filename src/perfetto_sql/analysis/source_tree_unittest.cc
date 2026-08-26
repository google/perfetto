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

#include "src/perfetto_sql/analysis/source_tree.h"

#include <optional>

#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::perfetto_sql::analysis {
namespace {

TEST(SourceTreeAnalyzerTest, BuildsCrossModuleGraph) {
  base::TmpDirTree tree;
  tree.AddDir("producer");
  tree.AddFile("producer/tables.sql",
               "CREATE PERFETTO TABLE input_table AS SELECT 1 AS id;");
  tree.AddFile("consumer.sql",
               "INCLUDE PERFETTO MODULE producer.tables;\n"
               "CREATE PERFETTO VIEW output_view AS "
               "SELECT id FROM input_table;");

  SourceTreeAnalyzer analyzer;
  analyzer.AddTree(tree.path());
  base::StatusOr<Program> program = analyzer.Analyze();

  ASSERT_TRUE(program.ok()) << program.status().c_message();
  ASSERT_THAT(program->modules(), testing::SizeIs(2));
  std::optional<SymbolId> output = program->FindSymbol("output_view");
  ASSERT_TRUE(output.has_value());
  const Symbol& symbol = program->symbol(*output);
  ASSERT_THAT(symbol.references, testing::SizeIs(1));
  EXPECT_EQ(program->symbol(symbol.references.front().symbol_id).name,
            "input_table");
  EXPECT_TRUE(symbol.unresolved_references.empty());
}

TEST(SourceTreeAnalyzerTest, ReportsTreeDiscoveryFailure) {
  base::TmpDirTree tree;
  SourceTreeAnalyzer analyzer;
  analyzer.AddTree(tree.path() + "/missing");

  base::StatusOr<Program> program = analyzer.Analyze();

  EXPECT_FALSE(program.ok());
}

}  // namespace
}  // namespace perfetto::perfetto_sql::analysis
