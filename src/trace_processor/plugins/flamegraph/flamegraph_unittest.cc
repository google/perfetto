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

#include "src/trace_processor/plugins/flamegraph/flamegraph.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/slab.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::flamegraph {
namespace {

struct Frame {
  std::string name;
  std::optional<uint32_t> parent;
  int64_t value;
};

template <typename T>
core::Tree::Column ColumnFromValues(const std::vector<T>& values) {
  auto column =
      core::Tree::Column::Create<T>(static_cast<uint32_t>(values.size()));
  memcpy(column.data.begin(), values.data(), values.size() * sizeof(T));
  return column;
}

core::Tree MakeTree(StringPool* pool, const std::vector<Frame>& frames) {
  core::Tree tree;
  tree.row_count = static_cast<uint32_t>(frames.size());
  tree.parent = core::Slab<uint32_t>::Alloc(frames.size());
  std::vector<StringPool::Id> names;
  std::vector<int64_t> values;
  for (uint32_t i = 0; i < frames.size(); ++i) {
    tree.parent[i] = frames[i].parent.value_or(core::Tree::kNullParent);
    names.push_back(pool->InternString(base::StringView(frames[i].name)));
    values.push_back(frames[i].value);
  }
  tree.names = {"name", "value"};
  tree.columns.push_back(ColumnFromValues(names));
  tree.columns.push_back(ColumnFromValues(values));
  return tree;
}

Config MakeConfig(const core::Tree& tree, StringPool& pool) {
  Config config(pool);
  config.name = &tree.columns[0];
  config.value_columns.push_back(&tree.columns[1]);
  return config;
}

template <typename T>
T Value(const core::Tree& tree, const char* name, uint32_t row) {
  auto column = tree.Find(name);
  PERFETTO_CHECK(column);
  return (*column)->unchecked_data<T>()[row];
}

TEST(FlamegraphTest, TopDownMergesSiblings) {
  StringPool pool;
  core::Tree input = MakeTree(
      &pool,
      {{"main", std::nullopt, 1}, {"b", 0, 2}, {"b", 0, 3}, {"c", 1, 4}});
  Config config = MakeConfig(input, pool);
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 3u);
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 0)).ToStdString(),
            "main");
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 0), 10);
  EXPECT_EQ(Value<int64_t>(*result, "self_value", 1), 5);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 1), 9);
  EXPECT_EQ(result->parent[2], 1u);
}

TEST(FlamegraphTest, BottomUp) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"a", 0, 2}, {"b", 1, 4}});
  Config config = MakeConfig(input, pool);
  config.view = Config::View::kBottomUp;
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 6u);
  EXPECT_EQ(Value<int64_t>(*result, "depth", 0), -1);
  EXPECT_EQ(Value<int64_t>(*result, "self_value", 4), 0);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 4), 4);
}

TEST(FlamegraphTest, RejectsIntegerOverflow) {
  StringPool pool;
  {
    core::Tree input = MakeTree(
        &pool, {{"main", std::nullopt, std::numeric_limits<int64_t>::max()},
                {"main", std::nullopt, 1}});
    Config config = MakeConfig(input, pool);
    EXPECT_FALSE(Build(std::move(input), config).ok());
  }
  {
    core::Tree input = MakeTree(
        &pool, {{"main", std::nullopt, std::numeric_limits<int64_t>::max()},
                {"child", 0, 1}});
    Config config = MakeConfig(input, pool);
    EXPECT_FALSE(Build(std::move(input), config).ok());
  }
}

TEST(FlamegraphTest, HideFrameFoldsValue) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"hide", 0, 2}, {"b", 1, 4}});
  Config config = MakeConfig(input, pool);
  config.hide_frame_filters.push_back(base::Regex::CreateOrCheck("hide"));
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 2u);
  EXPECT_EQ(Value<int64_t>(*result, "self_value", 0), 3);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 0), 7);
}

}  // namespace
}  // namespace perfetto::trace_processor::flamegraph
