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

#include "src/trace_processor/core/tree/tree_path_interner.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/tree/tree_from_dataframe.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

base::MurmurHashCombiner Hash(int64_t value) {
  base::MurmurHashCombiner hash;
  hash.Combine(value);
  return hash;
}

TEST(TreePathInternerTest, InternsByParentAndKey) {
  TreePathInterner tree(4);
  const uint32_t root = tree.Intern(Tree::kNullParent, Hash(1), 10);
  EXPECT_EQ(tree.Intern(Tree::kNullParent, Hash(1), 20), root);

  const uint32_t child = tree.Intern(root, Hash(2), 30);
  EXPECT_EQ(tree.Intern(root, Hash(2), 40), child);
  EXPECT_NE(tree.Intern(Tree::kNullParent, Hash(2), 50), child);

  EXPECT_EQ(tree.parent(child), root);
  EXPECT_EQ(tree.representative_row(root), 10u);
  EXPECT_EQ(tree.representative_row(child), 30u);
}

TEST(TreeFromDataframeTest, ReordersNullableColumn) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id", "value"}, &pool,
                                           options);

  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1}));
  builder.PushNull(2);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{0}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(2, int64_t{30}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{0}));
  ASSERT_TRUE(builder.PushNonNull(2, int64_t{20}));

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  ASSERT_EQ(tree.row_count, 3u);
  ASSERT_EQ(tree.columns.size(), 3u);
  EXPECT_THAT(tree.parent, testing::ElementsAre(Tree::kNullParent, 0u, 1u));

  const Tree::Column& values = tree.columns[2];
  EXPECT_EQ(values.unchecked_span<int64_t>()[0], 30);
  EXPECT_EQ(values.unchecked_span<int64_t>()[1], 20);
  EXPECT_TRUE(values.null_bv.is_set(0));
  EXPECT_TRUE(values.null_bv.is_set(1));
  EXPECT_FALSE(values.null_bv.is_set(2));
}

TEST(TreeFromDataframeTest, OrdersParentsBeforeChildrenDeterministically) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  const int64_t ids[] = {30, 10, 21, 20, 11, 12};
  const std::optional<int64_t> parents[] = {10,           std::nullopt, 20,
                                            std::nullopt, 10,           10};
  for (uint32_t row = 0; row < std::size(ids); ++row) {
    ASSERT_TRUE(builder.PushNonNull(0, ids[row]));
    if (parents[row]) {
      ASSERT_TRUE(builder.PushNonNull(1, *parents[row]));
    } else {
      builder.PushNull(1);
    }
  }

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  EXPECT_THAT(tree.columns[0].unchecked_span<int64_t>(),
              testing::ElementsAre(10, 30, 20, 21, 11, 12));
  EXPECT_THAT(tree.parent, testing::ElementsAre(Tree::kNullParent, 0u,
                                                Tree::kNullParent, 2u, 0u, 0u));
}

TEST(TreeFromDataframeTest, RejectsSelfLoop) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1}));

  auto result = BuildTree(std::move(builder));
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(), testing::HasSubstr("cycle detected"));
}

TEST(TreeFromDataframeTest, RejectsCycle) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{2}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1}));

  auto result = BuildTree(std::move(builder));
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(), testing::HasSubstr("cycle detected"));
}

}  // namespace
}  // namespace perfetto::trace_processor::core
