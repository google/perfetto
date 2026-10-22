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

#include "src/perfetto_sql/analysis/program.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::perfetto_sql::analysis {
namespace {

TEST(ProgramTest, OwnsNamesAndResolvesReferences) {
  Program program = [] {
    ProgramBuilder builder;
    ModuleId producer = builder.AddModule("producer", "producer.sql");
    ModuleId consumer = builder.AddModule("consumer", "consumer.sql");
    builder.AddSymbol(producer, "input_table", SymbolKind::kTable);
    SymbolId view =
        builder.AddSymbol(consumer, "output_view", SymbolKind::kView);
    builder.AddDeclaredInclude(consumer, "producer");
    builder.AddReference(view, "input_table", ReferenceKind::kRelation);
    builder.AddReference(view, "external_fn", ReferenceKind::kFunction);
    return builder.Build();
  }();

  ASSERT_THAT(program.modules(), testing::SizeIs(2));
  ASSERT_THAT(program.symbols(), testing::SizeIs(2));
  EXPECT_EQ(program.module(ModuleId{1}).name, "consumer");
  EXPECT_EQ(program.symbol(SymbolId{0}).name, "input_table");

  const Symbol& view = program.symbol(SymbolId{1});
  ASSERT_THAT(view.references, testing::SizeIs(1));
  EXPECT_EQ(view.references.front().symbol_id, SymbolId{0});
  EXPECT_EQ(view.references.front().kind, ReferenceKind::kRelation);
  ASSERT_THAT(view.unresolved_references, testing::SizeIs(1));
  EXPECT_EQ(view.unresolved_references.front().name, "external_fn");
  EXPECT_EQ(program.FindSymbol("output_view"), SymbolId{1});
}

TEST(ProgramTest, IgnoresReferencesWithinAModule) {
  ProgramBuilder builder;
  ModuleId module = builder.AddModule("module", "module.sql");
  builder.AddSymbol(module, "table", SymbolKind::kTable);
  SymbolId view = builder.AddSymbol(module, "view", SymbolKind::kView);
  builder.AddReference(view, "table", ReferenceKind::kRelation);
  Program program = builder.Build();

  EXPECT_TRUE(program.symbol(view).references.empty());
  EXPECT_TRUE(program.symbol(view).unresolved_references.empty());
}

}  // namespace
}  // namespace perfetto::perfetto_sql::analysis
