/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/tree/tree_builder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_to_string.h"
#include "src/trace_processor/core/interpreter/test_utils.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/slab.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::tree {
namespace {

using interpreter::EqualsIgnoringWhitespace;

class TreeBuilderBytecodeTest : public testing::Test {
 protected:
  static std::string FormatBytecode(const TreeTransformationBuilder& builder) {
    std::string result;
    for (const auto& bc : builder.GetBytecodeForTesting()) {
      result += interpreter::ToString(bc) + "\n";
    }
    return result;
  }

  std::unique_ptr<Tree> CreateTreeWithColumns(
      std::vector<uint32_t> parents,
      const std::vector<std::string>& col_names,
      std::vector<std::vector<int64_t>> col_data) {
    auto tree = std::make_unique<Tree>();
    tree->parents = Slab<uint32_t>::Alloc(parents.size());
    for (uint32_t i = 0; i < parents.size(); ++i) {
      tree->parents[i] = parents[i];
    }

    if (!col_names.empty()) {
      dataframe::AdhocDataframeBuilder builder(col_names, &pool_);
      for (uint32_t row = 0; row < parents.size(); ++row) {
        for (uint32_t col = 0; col < col_names.size(); ++col) {
          builder.PushNonNull(col, col_data[col][row]);
        }
      }
      auto df_result = std::move(builder).Build();
      EXPECT_TRUE(df_result.ok()) << df_result.status().message();
      tree->columns = std::move(df_result.value());
    }
    return tree;
  }

  StringPool pool_;
};

TEST_F(TreeBuilderBytecodeTest, NoTransformations) {
  auto tree = CreateTreeWithColumns({Tree::kNoParent, 0, 0, 1}, {"value"},
                                    {{10, 20, 30, 40}});

  TreeTransformationBuilder builder(std::move(tree));

  // No transformations, bytecode should be empty.
  EXPECT_THAT(FormatBytecode(builder), EqualsIgnoringWhitespace(""));
}

TEST_F(TreeBuilderBytecodeTest, SingleFilter) {
  auto tree = CreateTreeWithColumns({Tree::kNoParent, 0, 0, 1}, {"value"},
                                    {{10, 20, 30, 40}});

  TreeTransformationBuilder builder(std::move(tree));
  ASSERT_TRUE(builder.Filter(
      "value", TreeTransformationBuilder::FilterOp(core::Eq{}), int64_t{20}));

  // Single filter should generate:
  // 1. MakeParentToChildTreeStructure (CSR construction)
  // 2. FilterTree
  EXPECT_THAT(FormatBytecode(builder), EqualsIgnoringWhitespace(R"(
    MakeParentToChildTreeStructure: [source_register=Register(0), update_register=Register(1)]
    FilterTree: [source_register=Register(1), filter_register=Register(2), update_register=Register(0)]
  )"));
}

TEST_F(TreeBuilderBytecodeTest, MultipleFilters) {
  auto tree =
      CreateTreeWithColumns({Tree::kNoParent, 0, 0, 1}, {"value", "type"},
                            {{10, 20, 30, 40}, {1, 2, 1, 2}});

  TreeTransformationBuilder builder(std::move(tree));
  ASSERT_TRUE(builder.Filter(
      "value", TreeTransformationBuilder::FilterOp(core::Eq{}), int64_t{20}));
  ASSERT_TRUE(builder.Filter(
      "type", TreeTransformationBuilder::FilterOp(core::Eq{}), int64_t{1}));

  // Multiple filters should:
  // 1. Build CSR once
  // 2. Apply first filter
  // 3. Apply second filter (CSR already valid)
  EXPECT_THAT(FormatBytecode(builder), EqualsIgnoringWhitespace(R"(
    MakeParentToChildTreeStructure: [source_register=Register(0), update_register=Register(1)]
    FilterTree: [source_register=Register(1), filter_register=Register(2), update_register=Register(0)]
    FilterTree: [source_register=Register(1), filter_register=Register(3), update_register=Register(0)]
  )"));
}

TEST_F(TreeBuilderBytecodeTest, FilterNonExistentColumn) {
  auto tree =
      CreateTreeWithColumns({Tree::kNoParent, 0}, {"value"}, {{10, 20}});

  TreeTransformationBuilder builder(std::move(tree));

  // Filter on non-existent column should return false and add no bytecode.
  EXPECT_FALSE(builder.Filter("nonexistent",
                              TreeTransformationBuilder::FilterOp(core::Eq{}),
                              int64_t{10}));
  EXPECT_THAT(FormatBytecode(builder), EqualsIgnoringWhitespace(""));
}

TEST_F(TreeBuilderBytecodeTest, FilterOnTreeWithNoColumns) {
  auto tree = std::make_unique<Tree>();
  tree->parents = Slab<uint32_t>::Alloc(3);
  tree->parents[0] = Tree::kNoParent;
  tree->parents[1] = 0;
  tree->parents[2] = 0;

  TreeTransformationBuilder builder(std::move(tree));

  // Filter on tree with no columns should return false and add no bytecode.
  EXPECT_FALSE(builder.Filter(
      "any", TreeTransformationBuilder::FilterOp(core::Eq{}), int64_t{1}));
  EXPECT_THAT(FormatBytecode(builder), EqualsIgnoringWhitespace(""));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::tree
