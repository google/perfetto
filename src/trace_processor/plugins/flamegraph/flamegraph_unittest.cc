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
#include "src/trace_processor/core/tree/tree_column_ops.h"
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

template <typename T>
void AddColumn(core::Tree* tree,
               const char* name,
               const std::vector<T>& values) {
  tree->names.emplace_back(name);
  tree->columns.push_back(ColumnFromValues(values));
}

void AddStringColumn(core::Tree* tree,
                     StringPool* pool,
                     const char* name,
                     const std::vector<std::string>& values) {
  std::vector<StringPool::Id> ids;
  ids.reserve(values.size());
  for (const std::string& value : values) {
    ids.push_back(pool->InternString(base::StringView(value)));
  }
  AddColumn(tree, name, ids);
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

TEST(FlamegraphTest, LayoutPacksWidestFirstBreadthFirst) {
  StringPool pool;
  core::Tree input = MakeTree(
      &pool,
      {{"main", std::nullopt, 4}, {"a", 0, 3}, {"b", 0, 6}, {"c", 1, 2}});
  Config config = MakeConfig(input, pool);
  auto result = Build(input, config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 4u);

  auto cumulative = result->Find("cumulative_value");
  auto depth = result->Find("depth");
  ASSERT_TRUE(cumulative && depth);
  Layout layout = ComputeLayout(*result, **cumulative, **depth);

  auto name_at = [&](uint32_t row) {
    return pool.Get(Value<StringPool::Id>(*result, "name", layout.node[row]))
        .ToStdString();
  };
  EXPECT_EQ(name_at(0), "main");
  EXPECT_EQ(name_at(1), "b");
  EXPECT_EQ(name_at(2), "a");
  EXPECT_EQ(name_at(3), "c");
  EXPECT_EQ(layout.parent_row[0], core::Tree::kNullParent);
  EXPECT_EQ(layout.parent_row[1], 0u);
  EXPECT_EQ(layout.parent_row[2], 0u);
  EXPECT_EQ(layout.parent_row[3], 2u);
  EXPECT_DOUBLE_EQ(layout.x_start[0], 0.0);
  EXPECT_DOUBLE_EQ(layout.x_start[1], 0.0);
  EXPECT_DOUBLE_EQ(layout.x_start[2], 6.0);
  EXPECT_DOUBLE_EQ(layout.x_start[3], 6.0);
}

TEST(FlamegraphTest, StringAggregates) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"x", 0, 2}, {"x", 0, 3}});
  AddStringColumn(&input, &pool, "source", {"root", "a", "b"});
  AddColumn(&input, "num", std::vector<int64_t>{5, 7, 8});
  AddColumn(&input, "frac", std::vector<double>{1.5, 2.0, 2.0});
  Config config = MakeConfig(input, pool);
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kOneOrSummary, "summary"});
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kConcatWithComma, "concatenated"});
  config.aggregate_columns.push_back(
      {&input.columns[3], Config::Aggregate::kOneOrSummary, "num_summary"});
  config.aggregate_columns.push_back(
      {&input.columns[3], Config::Aggregate::kConcatWithComma, "nums"});
  config.aggregate_columns.push_back(
      {&input.columns[4], Config::Aggregate::kOneOrSummary, "frac_summary"});
  config.aggregate_columns.push_back(
      {&input.columns[4], Config::Aggregate::kConcatWithComma, "fracs"});

  auto result = Build(input, config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 2u);
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "summary", 0)).ToStdString(),
      "root");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "summary", 1)).ToStdString(),
      "a and 2 others");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "concatenated", 1)).ToStdString(),
      "a,b");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "num_summary", 0)).ToStdString(),
      "5");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "num_summary", 1)).ToStdString(),
      "7 and 2 others");
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "nums", 1)).ToStdString(),
            "7,8");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "frac_summary", 0)).ToStdString(),
      "1.5");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "frac_summary", 1)).ToStdString(),
      "2.0");
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "fracs", 1)).ToStdString(),
            "2.0,2.0");
}

// The dataframe layer materializes a column whose rows are all null as a
// null-typed column. Every aggregate mode over such a column produces an
// all-null output.
TEST(FlamegraphTest, AllNullAggregates) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"x", 0, 2}, {"x", 0, 3}});
  input.names.emplace_back("source");
  input.columns.push_back(core::Tree::Column::CreateNull(3));
  Config config = MakeConfig(input, pool);
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kOneOrSummary, "summary"});
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kConcatWithComma, "concatenated"});
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kSum, "summed"});

  auto result = Build(input, config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 2u);
  for (uint32_t row = 0; row < result->row_count; ++row) {
    for (const char* name : {"summary", "concatenated", "summed"}) {
      auto output = result->Find(name);
      ASSERT_TRUE(output);
      ASSERT_TRUE((*output)->null_bv.size() > 0);
      EXPECT_FALSE((*output)->null_bv.is_set(row));
    }
  }
}

// A name column holding no values at all is typed Null rather than String,
// e.g. a heap graph whose class names were all stripped. Every frame is then
// unnamed, which is still a well-formed flamegraph.
TEST(FlamegraphTest, AllNullNames) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"a", 0, 2}, {"b", 0, 3}});
  input.columns[0] = core::Tree::Column::CreateNull(3);
  Config config = MakeConfig(input, pool);

  auto result = Build(input, config);
  ASSERT_TRUE(result.ok());
  // Both children are unnamed, so they merge into a single child frame.
  ASSERT_EQ(result->row_count, 2u);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 0), 6);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 1), 5);
  auto name = result->Find("name");
  ASSERT_TRUE(name);
  ASSERT_TRUE((*name)->null_bv.size() > 0);
  for (uint32_t row = 0; row < result->row_count; ++row) {
    EXPECT_FALSE((*name)->null_bv.is_set(row));
  }
}

// Filters read the name of every frame, which must not touch the absent
// payload of a Null-typed column. Nothing can match, so nothing is retained.
TEST(FlamegraphTest, AllNullNamesWithFilters) {
  StringPool pool;
  core::Tree input = MakeTree(&pool, {{"main", std::nullopt, 1}, {"a", 0, 2}});
  input.columns[0] = core::Tree::Column::CreateNull(2);
  Config config = MakeConfig(input, pool);
  config.show_stack_filters.push_back(base::Regex::CreateOrCheck("^main$"));

  auto result = Build(input, config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->row_count, 0u);
}

TEST(FlamegraphTest, BottomUp) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"a", 0, 2}, {"b", 1, 4}});
  Config config = MakeConfig(input, pool);
  config.view = Config::View(Config::BottomUp{});
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 6u);
  EXPECT_EQ(Value<int64_t>(*result, "depth", 0), -1);
  EXPECT_EQ(Value<int64_t>(*result, "self_value", 4), 0);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 4), 4);
}

TEST(FlamegraphTest, PivotAndFromFrameViews) {
  StringPool pool;
  const std::vector<Frame> frames = {{"main", std::nullopt, 1},
                                     {"pivot", 0, 2},
                                     {"leaf", 1, 4},
                                     {"other", 0, 8}};
  for (bool pivot : {false, true}) {
    core::Tree input = MakeTree(&pool, frames);
    Config config = MakeConfig(input, pool);
    config.view = pivot ? Config::View(Config::Pivot{})
                        : Config::View(Config::FromFrame{});
    config.view_pattern = base::Regex::CreateOrCheck("^pivot$");
    auto result = Build(std::move(input), config);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->row_count, pivot ? 4u : 2u);
    EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 0)).ToStdString(),
              "pivot");
    EXPECT_EQ(Value<int64_t>(*result, "depth", 0), 1);
    EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 1)).ToStdString(),
              "leaf");
    if (pivot) {
      EXPECT_EQ(
          pool.Get(Value<StringPool::Id>(*result, "name", 2)).ToStdString(),
          "pivot");
      EXPECT_EQ(
          pool.Get(Value<StringPool::Id>(*result, "name", 3)).ToStdString(),
          "main");
      EXPECT_EQ(Value<int64_t>(*result, "depth", 3), -2);
    }
  }
}

TEST(FlamegraphTest, ShowAndHideStackFilters) {
  StringPool pool;
  core::Tree input = MakeTree(&pool, {{"main", std::nullopt, 1},
                                      {"show", 0, 2},
                                      {"leaf", 1, 4},
                                      {"hidden", 0, 8},
                                      {"hidden_leaf", 3, 16}});
  Config config = MakeConfig(input, pool);
  config.show_stack_filters.push_back(base::Regex::CreateOrCheck("^leaf$"));
  config.hide_stack_filters.push_back(
      base::Regex::CreateOrCheck("^hidden_leaf$"));
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 3u);
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 0)).ToStdString(),
            "main");
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 1)).ToStdString(),
            "show");
  EXPECT_EQ(pool.Get(Value<StringPool::Id>(*result, "name", 2)).ToStdString(),
            "leaf");
}

TEST(FlamegraphTest, RejectsInvalidConfig) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"child", 0, 2}});

  Config missing_value(pool);
  missing_value.name = &input.columns[0];
  EXPECT_FALSE(Build(input, missing_value).ok());

  Config non_numeric = MakeConfig(input, pool);
  non_numeric.value_columns = {&input.columns[0]};
  EXPECT_FALSE(Build(input, non_numeric).ok());

  Config duplicate_output = MakeConfig(input, pool);
  duplicate_output.aggregate_columns.push_back(
      {&input.columns[1], Config::Aggregate::kSum, "self_value"});
  EXPECT_FALSE(Build(input, duplicate_output).ok());
}

TEST(FlamegraphTest, RejectsNegativeValues) {
  StringPool pool;
  // Negative values are rejected in downward views (TOP_DOWN).
  {
    core::Tree input = MakeTree(&pool, {{"main", std::nullopt, -1}});
    Config config = MakeConfig(input, pool);
    EXPECT_FALSE(Build(std::move(input), config).ok());
  }
  // Negative values are rejected in upward views (BOTTOM_UP) too.
  {
    core::Tree input = MakeTree(&pool, {{"main", std::nullopt, -1}});
    Config config = MakeConfig(input, pool);
    config.view = Config::View(Config::BottomUp{});
    EXPECT_FALSE(Build(std::move(input), config).ok());
  }
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

TEST(FlamegraphTest, EmptyLayout) {
  core::Tree tree;
  core::Tree::Column cumulative = core::Tree::Column::Create<int64_t>(0);
  core::Tree::Column depth = core::Tree::Column::Create<int64_t>(0);
  Layout layout = ComputeLayout(tree, cumulative, depth);
  EXPECT_EQ(layout.node.size(), 0u);
  EXPECT_EQ(layout.parent_row.size(), 0u);
  EXPECT_EQ(layout.x_start.size(), 0u);
}

TEST(FlamegraphTest, HideFrameFoldsValue) {
  StringPool pool;
  core::Tree input =
      MakeTree(&pool, {{"main", std::nullopt, 1}, {"hide", 0, 2}, {"b", 1, 4}});
  AddStringColumn(&input, &pool, "source", {"root", "hidden", "child"});
  Config config = MakeConfig(input, pool);
  config.hide_frame_filters.push_back(base::Regex::CreateOrCheck("hide"));
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kOneOrSummary, "summary"});
  config.aggregate_columns.push_back(
      {&input.columns[2], Config::Aggregate::kConcatWithComma, "concatenated"});
  auto result = Build(std::move(input), config);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->row_count, 2u);
  EXPECT_EQ(Value<int64_t>(*result, "self_value", 0), 3);
  EXPECT_EQ(Value<int64_t>(*result, "cumulative_value", 0), 7);
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "summary", 0)).ToStdString(),
      "root and 2 others");
  EXPECT_EQ(
      pool.Get(Value<StringPool::Id>(*result, "concatenated", 0)).ToStdString(),
      "root,hidden");
}

}  // namespace
}  // namespace perfetto::trace_processor::flamegraph
