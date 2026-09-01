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

TEST(TreeTest, DefaultConstructedTreeResolvesIdentityIds) {
  // The index defaults to identity: an empty tree has no rows, so every id
  // resolves to absent, and a tree without an id column would resolve row
  // indices directly.
  Tree tree;
  ASSERT_TRUE(tree.id_index.identity_ids);
  EXPECT_EQ(tree.FindRow(0), Tree::kNullParent);
  EXPECT_EQ(tree.FindRow(-1), Tree::kNullParent);
}

TEST(TreeFromDataframeTest, FindRowIdentityIds) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  // Ids equal their row index, so the index is identity and needs no storage.
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{0}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{0}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{0}));

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  ASSERT_TRUE(tree.id_index.identity_ids);

  EXPECT_EQ(tree.FindRow(0), 0u);
  EXPECT_EQ(tree.FindRow(1), 1u);
  EXPECT_EQ(tree.FindRow(2), 2u);
  EXPECT_EQ(tree.FindRow(3), Tree::kNullParent);
  EXPECT_EQ(tree.FindRow(-1), Tree::kNullParent);
}

TEST(TreeFromDataframeTest, FindRowDenseIds) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  // A compact non-identity uint32 id range is stored as a direct index
  // vector (max_id + 1 <= 2 * row_count).
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{3}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1}));

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  ASSERT_FALSE(tree.id_index.identity_ids);
  ASSERT_FALSE(tree.id_index.dense.empty());

  EXPECT_EQ(tree.FindRow(1), 0u);
  EXPECT_EQ(tree.FindRow(2), 1u);
  EXPECT_EQ(tree.FindRow(3), 2u);
  EXPECT_EQ(tree.FindRow(0), Tree::kNullParent);  // absent id
  EXPECT_EQ(tree.FindRow(4), Tree::kNullParent);  // beyond the dense range
  EXPECT_EQ(tree.FindRow(-1), Tree::kNullParent);
}

TEST(TreeFromDataframeTest, FindRowHashIds) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  // Sparse ids, including one beyond uint32, fall back to a hash map.
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{1000}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{5000000000}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1000}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{9000}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{1000}));

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  ASSERT_FALSE(tree.id_index.identity_ids);
  ASSERT_TRUE(tree.id_index.dense.empty());
  ASSERT_TRUE(tree.id_index.hash.has_value());

  EXPECT_EQ(tree.FindRow(1000), 0u);
  EXPECT_EQ(tree.FindRow(5000000000), 1u);
  EXPECT_EQ(tree.FindRow(9000), 2u);
  EXPECT_EQ(tree.FindRow(1001), Tree::kNullParent);
  EXPECT_EQ(tree.FindRow(-1), Tree::kNullParent);
}

TEST(TreeFromDataframeTest, ReorderRebuildsIdIndex) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  // Child rows precede their parents in the input, so the topological reorder
  // invalidates the input-order index and it is rebuilt from the output id
  // column.
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{5}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{3}));
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{3}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{4}));
  ASSERT_TRUE(builder.PushNonNull(1, int64_t{3}));

  auto result = BuildTree(std::move(builder));
  ASSERT_TRUE(result.ok()) << result.status().message();
  Tree tree = std::move(result.value());
  ASSERT_FALSE(tree.id_index.identity_ids);
  ASSERT_FALSE(tree.id_index.dense.empty());

  EXPECT_THAT(tree.columns[0].unchecked_span<int64_t>(),
              testing::ElementsAre(3, 5, 4));
  EXPECT_EQ(tree.FindRow(3), 0u);
  EXPECT_EQ(tree.FindRow(5), 1u);
  EXPECT_EQ(tree.FindRow(4), 2u);
  EXPECT_EQ(tree.FindRow(6), Tree::kNullParent);
}

TEST(TreeFromDataframeTest, RejectsDuplicateIds) {
  StringPool pool;
  dataframe::AdhocDataframeBuilder::Options options;
  options.types = {dataframe::AdhocColumnType::kInt64,
                   dataframe::AdhocColumnType::kInt64};
  options.nullability_type = dataframe::NullabilityType::kDenseNull;
  dataframe::AdhocDataframeBuilder builder({"id", "parent_id"}, &pool, options);

  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  builder.PushNull(1);
  ASSERT_TRUE(builder.PushNonNull(0, int64_t{2}));
  builder.PushNull(1);

  auto result = BuildTree(std::move(builder));
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(), testing::HasSubstr("duplicate id"));
}

}  // namespace
}  // namespace perfetto::trace_processor::core
