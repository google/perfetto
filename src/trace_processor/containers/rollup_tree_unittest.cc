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

#include "src/trace_processor/containers/rollup_tree.h"

#include <cstdint>
#include <string>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(RollupTreeTest, EmptyTree) {
  RollupTree tree({"category", "item"}, 1);

  EXPECT_EQ(tree.hierarchy_cols().size(), 2u);
  EXPECT_EQ(tree.num_aggregates(), 1u);
  EXPECT_EQ(tree.total_nodes(), 0);

  RollupFlattenOptions opts;
  opts.denylist_mode = true;  // Expand all
  auto rows = tree.GetRows(opts);

  // Only root node should be present
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].id, 0);
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[0].child_count, 0);
}

TEST(RollupTreeTest, AddSingleLevel) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{100})});
  tree.AddRow(0, {RollupValue("vegetable")}, {RollupValue(int64_t{50})});
  tree.SetRootAggregates({RollupValue(int64_t{150})});

  EXPECT_EQ(tree.total_nodes(), 2);

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 3u);

  // Root (sorted by agg DESC by default)
  EXPECT_EQ(rows[0].depth, 0);
  EXPECT_EQ(rows[0].child_count, 2);

  // fruit (100)
  EXPECT_EQ(rows[1].depth, 1);
  EXPECT_EQ(std::get<std::string>(rows[1].hierarchy_values[0]), "fruit");
  EXPECT_EQ(std::get<int64_t>(rows[1].aggregates[0]), 100);

  // vegetable (50)
  EXPECT_EQ(rows[2].depth, 1);
  EXPECT_EQ(std::get<std::string>(rows[2].hierarchy_values[0]), "vegetable");
  EXPECT_EQ(std::get<int64_t>(rows[2].aggregates[0]), 50);
}

TEST(RollupTreeTest, AddTwoLevels) {
  RollupTree tree({"category", "item"}, 1);

  // Level 0 (category totals)
  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{45})});

  // Level 1 (item details)
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("apple")},
              {RollupValue(int64_t{30})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("banana")},
              {RollupValue(int64_t{15})});

  tree.SetRootAggregates({RollupValue(int64_t{45})});

  EXPECT_EQ(tree.total_nodes(), 3);

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 4u);

  // Root
  EXPECT_EQ(rows[0].depth, 0);

  // fruit
  EXPECT_EQ(rows[1].depth, 1);
  EXPECT_EQ(std::get<std::string>(rows[1].hierarchy_values[0]), "fruit");

  // apple (30) - sorted by agg DESC
  EXPECT_EQ(rows[2].depth, 2);
  EXPECT_EQ(std::get<std::string>(rows[2].hierarchy_values[1]), "apple");

  // banana (15)
  EXPECT_EQ(rows[3].depth, 2);
  EXPECT_EQ(std::get<std::string>(rows[3].hierarchy_values[1]), "banana");
}

TEST(RollupTreeTest, IntegerHierarchyValues) {
  RollupTree tree({"region_id", "store_id"}, 1);

  tree.AddRow(0, {RollupValue(int64_t{1})}, {RollupValue(int64_t{100})});
  tree.AddRow(0, {RollupValue(int64_t{2})}, {RollupValue(int64_t{50})});
  tree.AddRow(1, {RollupValue(int64_t{1}), RollupValue(int64_t{101})},
              {RollupValue(int64_t{60})});
  tree.AddRow(1, {RollupValue(int64_t{1}), RollupValue(int64_t{102})},
              {RollupValue(int64_t{40})});

  tree.SetRootAggregates({RollupValue(int64_t{150})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 5u);

  // Verify integer types are preserved
  EXPECT_TRUE(std::holds_alternative<int64_t>(rows[1].hierarchy_values[0]));
  EXPECT_EQ(std::get<int64_t>(rows[1].hierarchy_values[0]), 1);

  EXPECT_TRUE(std::holds_alternative<int64_t>(rows[2].hierarchy_values[1]));
  EXPECT_EQ(std::get<int64_t>(rows[2].hierarchy_values[1]), 101);
}

TEST(RollupTreeTest, NullHierarchyValues) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue(std::monostate{})}, {RollupValue(int64_t{100})});
  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{50})});

  tree.SetRootAggregates({RollupValue(int64_t{150})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.sort.agg_index = -1;  // Sort by name
  opts.sort.descending = false;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 3u);

  // NULL should sort first
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(rows[1].hierarchy_values[0]));

  // Then "fruit"
  EXPECT_EQ(std::get<std::string>(rows[2].hierarchy_values[0]), "fruit");
}

TEST(RollupTreeTest, RealHierarchyValues) {
  RollupTree tree({"price_tier"}, 1);

  tree.AddRow(0, {RollupValue(1.5)}, {RollupValue(int64_t{100})});
  tree.AddRow(0, {RollupValue(2.5)}, {RollupValue(int64_t{50})});

  tree.SetRootAggregates({RollupValue(int64_t{150})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 3u);

  // Verify double types are preserved
  EXPECT_TRUE(std::holds_alternative<double>(rows[1].hierarchy_values[0]));
  EXPECT_TRUE(std::holds_alternative<double>(rows[2].hierarchy_values[0]));
}

TEST(RollupTreeTest, ExpandCollapse) {
  RollupTree tree({"category", "item"}, 1);

  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{45})});
  tree.AddRow(0, {RollupValue("vegetable")}, {RollupValue(int64_t{25})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("apple")},
              {RollupValue(int64_t{30})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("banana")},
              {RollupValue(int64_t{15})});
  tree.AddRow(1, {RollupValue("vegetable"), RollupValue("carrot")},
              {RollupValue(int64_t{25})});

  tree.SetRootAggregates({RollupValue(int64_t{70})});

  // Expand only fruit (id=1)
  RollupFlattenOptions opts;
  opts.ids = {1};
  opts.denylist_mode = false;  // Allowlist mode
  auto rows = tree.GetRows(opts);

  // Should see: root, fruit, apple, banana, vegetable (collapsed)
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0].depth, 0);  // root
  EXPECT_EQ(rows[1].depth, 1);  // fruit
  EXPECT_EQ(rows[2].depth, 2);  // apple
  EXPECT_EQ(rows[3].depth, 2);  // banana
  EXPECT_EQ(rows[4].depth, 1);  // vegetable (no children shown)
}

TEST(RollupTreeTest, SortByAggregateAsc) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("a")}, {RollupValue(int64_t{30})});
  tree.AddRow(0, {RollupValue("b")}, {RollupValue(int64_t{10})});
  tree.AddRow(0, {RollupValue("c")}, {RollupValue(int64_t{20})});

  tree.SetRootAggregates({RollupValue(int64_t{60})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.sort.agg_index = 0;
  opts.sort.descending = false;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(std::get<int64_t>(rows[1].aggregates[0]), 10);  // b
  EXPECT_EQ(std::get<int64_t>(rows[2].aggregates[0]), 20);  // c
  EXPECT_EQ(std::get<int64_t>(rows[3].aggregates[0]), 30);  // a
}

TEST(RollupTreeTest, SortByName) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("cherry")}, {RollupValue(int64_t{10})});
  tree.AddRow(0, {RollupValue("apple")}, {RollupValue(int64_t{20})});
  tree.AddRow(0, {RollupValue("banana")}, {RollupValue(int64_t{30})});

  tree.SetRootAggregates({RollupValue(int64_t{60})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.sort.agg_index = -1;  // Sort by hierarchy value
  opts.sort.descending = false;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(std::get<std::string>(rows[1].hierarchy_values[0]), "apple");
  EXPECT_EQ(std::get<std::string>(rows[2].hierarchy_values[0]), "banana");
  EXPECT_EQ(std::get<std::string>(rows[3].hierarchy_values[0]), "cherry");
}

TEST(RollupTreeTest, Pagination) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("a")}, {RollupValue(int64_t{50})});
  tree.AddRow(0, {RollupValue("b")}, {RollupValue(int64_t{40})});
  tree.AddRow(0, {RollupValue("c")}, {RollupValue(int64_t{30})});
  tree.AddRow(0, {RollupValue("d")}, {RollupValue(int64_t{20})});
  tree.AddRow(0, {RollupValue("e")}, {RollupValue(int64_t{10})});

  tree.SetRootAggregates({RollupValue(int64_t{150})});

  // Get rows 2-3 (offset=2, limit=2)
  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.offset = 2;
  opts.limit = 2;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 2u);
  // Default sort is by agg DESC, so order is: root(150), a(50), b(40), c(30)...
  // offset=2 skips root and a, so we get b and c
  EXPECT_EQ(std::get<std::string>(rows[0].hierarchy_values[0]), "b");
  EXPECT_EQ(std::get<std::string>(rows[1].hierarchy_values[0]), "c");
}

TEST(RollupTreeTest, PaginationOffsetOnly) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("a")}, {RollupValue(int64_t{30})});
  tree.AddRow(0, {RollupValue("b")}, {RollupValue(int64_t{20})});
  tree.AddRow(0, {RollupValue("c")}, {RollupValue(int64_t{10})});

  tree.SetRootAggregates({RollupValue(int64_t{60})});

  // Offset with no limit (tests integer overflow fix)
  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.offset = 2;
  // limit stays at default (INT_MAX)
  auto rows = tree.GetRows(opts);

  // Total rows: root, a, b, c = 4
  // Offset 2 should give: b, c
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(std::get<std::string>(rows[0].hierarchy_values[0]), "b");
  EXPECT_EQ(std::get<std::string>(rows[1].hierarchy_values[0]), "c");
}

TEST(RollupTreeTest, MinMaxDepth) {
  RollupTree tree({"category", "item"}, 1);

  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{45})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("apple")},
              {RollupValue(int64_t{30})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("banana")},
              {RollupValue(int64_t{15})});

  tree.SetRootAggregates({RollupValue(int64_t{45})});

  // Exclude root (min_depth=1)
  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  opts.min_depth = 1;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].depth, 1);  // fruit (not root)

  // Only depth 1 (max_depth=1)
  opts.min_depth = 1;
  opts.max_depth = 1;
  rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].depth, 1);
  EXPECT_EQ(std::get<std::string>(rows[0].hierarchy_values[0]), "fruit");
}

TEST(RollupTreeTest, GetTotalRows) {
  RollupTree tree({"category"}, 1);

  tree.AddRow(0, {RollupValue("a")}, {RollupValue(int64_t{10})});
  tree.AddRow(0, {RollupValue("b")}, {RollupValue(int64_t{20})});
  tree.AddRow(0, {RollupValue("c")}, {RollupValue(int64_t{30})});

  tree.SetRootAggregates({RollupValue(int64_t{60})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;

  // Total should be 4 (root + 3 categories)
  EXPECT_EQ(tree.GetTotalRows(opts), 4);

  // With min_depth=1, should be 3
  opts.min_depth = 1;
  EXPECT_EQ(tree.GetTotalRows(opts), 3);
}

TEST(RollupTreeTest, ParentIdTracking) {
  RollupTree tree({"category", "item"}, 1);

  tree.AddRow(0, {RollupValue("fruit")}, {RollupValue(int64_t{45})});
  tree.AddRow(1, {RollupValue("fruit"), RollupValue("apple")},
              {RollupValue(int64_t{30})});

  tree.SetRootAggregates({RollupValue(int64_t{45})});

  RollupFlattenOptions opts;
  opts.denylist_mode = true;
  auto rows = tree.GetRows(opts);

  ASSERT_EQ(rows.size(), 3u);

  // Root has no parent
  EXPECT_EQ(rows[0].parent_id, -1);

  // fruit's parent is root (id=0)
  EXPECT_EQ(rows[1].parent_id, 0);

  // apple's parent is fruit
  EXPECT_EQ(rows[2].parent_id, rows[1].id);
}

}  // namespace
}  // namespace perfetto::trace_processor
