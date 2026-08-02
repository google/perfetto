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

#include "src/trace_processor/core/tree/tree_from_dataframe.h"

#include <cstdint>
#include <utility>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/tree/tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

TEST(TreeFromDataframeTest, PreservesRowOrderAndNullableColumn) {
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

  EXPECT_EQ(tree.parent[0], 2u);
  EXPECT_EQ(tree.parent[1], Tree::kNullParent);
  EXPECT_EQ(tree.parent[2], 1u);

  const Tree::Column& values = tree.columns[2];
  EXPECT_EQ(values.unchecked_span<int64_t>()[1], 30);
  EXPECT_EQ(values.unchecked_span<int64_t>()[2], 20);
  EXPECT_FALSE(values.null_bv.is_set(0));
  EXPECT_TRUE(values.null_bv.is_set(1));
  EXPECT_TRUE(values.null_bv.is_set(2));
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
