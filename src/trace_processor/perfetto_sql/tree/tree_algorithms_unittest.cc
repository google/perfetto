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

#include "src/trace_processor/perfetto_sql/tree/tree_algorithms.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::plugins::tree {
namespace {

using ::perfetto::trace_processor::StringPool;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;

// =============================================================================
// BuildTreeStructure tests
// =============================================================================

TEST(TreeAlgorithmsTest, BuildTreeStructure_Empty) {
  std::vector<TreeInputRow> rows;
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->node_ids, IsEmpty());
  EXPECT_THAT(result->parent_indices, IsEmpty());
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_SingleRoot) {
  std::vector<TreeInputRow> rows = {
      {1, kNullInt64},
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->node_ids, ElementsAre(1));
  EXPECT_THAT(result->parent_indices, ElementsAre(kNullUint32));
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_SimpleTree) {
  // Tree:
  //   1 (root)
  //   ├── 2
  //   └── 3
  //       └── 4
  std::vector<TreeInputRow> rows = {
      {1, kNullInt64},
      {2, 1},
      {3, 1},
      {4, 3},
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->node_ids, ElementsAre(1, 2, 3, 4));
  // Row 0 (id=1): no parent
  // Row 1 (id=2): parent is id=1, which is row 0
  // Row 2 (id=3): parent is id=1, which is row 0
  // Row 3 (id=4): parent is id=3, which is row 2
  EXPECT_EQ(result->parent_indices[0], kNullUint32);
  EXPECT_EQ(result->parent_indices[1], 0u);
  EXPECT_EQ(result->parent_indices[2], 0u);
  EXPECT_EQ(result->parent_indices[3], 2u);
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_MultipleRoots) {
  std::vector<TreeInputRow> rows = {
      {1, kNullInt64},
      {2, kNullInt64},
      {3, 1},
      {4, 2},
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_EQ(result->parent_indices[0], kNullUint32);
  EXPECT_EQ(result->parent_indices[1], kNullUint32);
  EXPECT_EQ(result->parent_indices[2], 0u);
  EXPECT_EQ(result->parent_indices[3], 1u);
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_DuplicateId) {
  std::vector<TreeInputRow> rows = {
      {1, kNullInt64}, {1, kNullInt64},  // Duplicate!
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().c_message(), testing::HasSubstr("Duplicate"));
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_OrphanNode) {
  // Node 2 references parent 999 which doesn't exist
  std::vector<TreeInputRow> rows = {
      {1, kNullInt64}, {2, 999},  // Orphan - parent doesn't exist
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Orphan becomes a root
  EXPECT_EQ(result->parent_indices[0], kNullUint32);
  EXPECT_EQ(result->parent_indices[1], kNullUint32);
}

TEST(TreeAlgorithmsTest, BuildTreeStructure_UnorderedInput) {
  // Input in arbitrary order
  std::vector<TreeInputRow> rows = {
      {4, 2},
      {2, 1},
      {1, kNullInt64},
      {3, 1},
  };
  auto result = BuildTreeStructure(rows);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Row 0 (id=4): parent is id=2, which is row 1
  // Row 1 (id=2): parent is id=1, which is row 2
  // Row 2 (id=1): no parent
  // Row 3 (id=3): parent is id=1, which is row 2
  EXPECT_EQ(result->parent_indices[0], 1u);
  EXPECT_EQ(result->parent_indices[1], 2u);
  EXPECT_EQ(result->parent_indices[2], kNullUint32);
  EXPECT_EQ(result->parent_indices[3], 2u);
}

// =============================================================================
// ComputeDepths tests
// =============================================================================

TEST(TreeAlgorithmsTest, ComputeDepths_Empty) {
  std::vector<uint32_t> parent_indices;
  auto depths = ComputeDepths(parent_indices);
  EXPECT_THAT(depths, IsEmpty());
}

TEST(TreeAlgorithmsTest, ComputeDepths_SingleRoot) {
  std::vector<uint32_t> parent_indices = {kNullUint32};
  auto depths = ComputeDepths(parent_indices);
  EXPECT_THAT(depths, ElementsAre(0));
}

TEST(TreeAlgorithmsTest, ComputeDepths_SimpleTree) {
  // Tree:
  //   0 (root, depth 0)
  //   ├── 1 (depth 1)
  //   └── 2 (depth 1)
  //       └── 3 (depth 2)
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      2,            // 3: child of 2
  };
  auto depths = ComputeDepths(parent_indices);
  EXPECT_THAT(depths, ElementsAre(0, 1, 1, 2));
}

TEST(TreeAlgorithmsTest, ComputeDepths_DeepChain) {
  // Chain: 0 -> 1 -> 2 -> 3 -> 4
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      1,            // 2: child of 1
      2,            // 3: child of 2
      3,            // 4: child of 3
  };
  auto depths = ComputeDepths(parent_indices);
  EXPECT_THAT(depths, ElementsAre(0, 1, 2, 3, 4));
}

TEST(TreeAlgorithmsTest, ComputeDepths_MultipleRoots) {
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      kNullUint32,  // 1: another root
      0,            // 2: child of 0
      1,            // 3: child of 1
  };
  auto depths = ComputeDepths(parent_indices);
  EXPECT_THAT(depths, ElementsAre(0, 0, 1, 1));
}

// =============================================================================
// TopologicalOrder tests
// =============================================================================

TEST(TreeAlgorithmsTest, TopologicalOrder_Empty) {
  std::vector<uint32_t> parent_indices;
  auto order = TopologicalOrder(parent_indices);
  EXPECT_THAT(order, IsEmpty());
}

TEST(TreeAlgorithmsTest, TopologicalOrder_SingleRoot) {
  std::vector<uint32_t> parent_indices = {kNullUint32};
  auto order = TopologicalOrder(parent_indices);
  EXPECT_THAT(order, ElementsAre(0));
}

TEST(TreeAlgorithmsTest, TopologicalOrder_SimpleTree) {
  // Tree:
  //   0 (root)
  //   ├── 1
  //   └── 2
  //       └── 3
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      2,            // 3: child of 2
  };
  auto order = TopologicalOrder(parent_indices);

  // Root must come first
  EXPECT_EQ(order[0], 0u);
  // Parents must come before children
  // Find positions
  auto pos_0 = std::find(order.begin(), order.end(), 0u) - order.begin();
  auto pos_1 = std::find(order.begin(), order.end(), 1u) - order.begin();
  auto pos_2 = std::find(order.begin(), order.end(), 2u) - order.begin();
  auto pos_3 = std::find(order.begin(), order.end(), 3u) - order.begin();

  EXPECT_LT(pos_0, pos_1);  // 0 before 1
  EXPECT_LT(pos_0, pos_2);  // 0 before 2
  EXPECT_LT(pos_2, pos_3);  // 2 before 3
  EXPECT_EQ(order.size(), 4u);
}

TEST(TreeAlgorithmsTest, TopologicalOrder_MultipleRoots) {
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      kNullUint32,  // 1: another root
      0,            // 2: child of 0
      1,            // 3: child of 1
  };
  auto order = TopologicalOrder(parent_indices);

  // Roots should come before their children
  auto pos_0 = std::find(order.begin(), order.end(), 0u) - order.begin();
  auto pos_1 = std::find(order.begin(), order.end(), 1u) - order.begin();
  auto pos_2 = std::find(order.begin(), order.end(), 2u) - order.begin();
  auto pos_3 = std::find(order.begin(), order.end(), 3u) - order.begin();

  EXPECT_LT(pos_0, pos_2);  // 0 before 2
  EXPECT_LT(pos_1, pos_3);  // 1 before 3
  EXPECT_EQ(order.size(), 4u);
}

// =============================================================================
// BuildChildrenMap tests
// =============================================================================

TEST(TreeAlgorithmsTest, BuildChildrenMap_Empty) {
  std::vector<uint32_t> parent_indices;
  auto children = BuildChildrenMap(parent_indices);
  EXPECT_THAT(children, IsEmpty());
}

TEST(TreeAlgorithmsTest, BuildChildrenMap_SimpleTree) {
  // Tree:
  //   0 (root)
  //   ├── 1
  //   └── 2
  //       └── 3
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0
      0,            // 1
      0,            // 2
      2,            // 3
  };
  auto children = BuildChildrenMap(parent_indices);
  EXPECT_THAT(children[0], ElementsAre(1, 2));
  EXPECT_THAT(children[1], IsEmpty());
  EXPECT_THAT(children[2], ElementsAre(3));
  EXPECT_THAT(children[3], IsEmpty());
}

// =============================================================================
// ApplyAggregation tests
// =============================================================================

TEST(TreeAlgorithmsTest, ApplyAggregation_Int64_Min) {
  std::vector<int64_t> values = {5, 2, 8, 1, 9};
  EXPECT_EQ(ApplyAggregation(values, TreeAggType::kMin), 1);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Int64_Max) {
  std::vector<int64_t> values = {5, 2, 8, 1, 9};
  EXPECT_EQ(ApplyAggregation(values, TreeAggType::kMax), 9);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Int64_Sum) {
  std::vector<int64_t> values = {1, 2, 3, 4, 5};
  EXPECT_EQ(ApplyAggregation(values, TreeAggType::kSum), 15);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Int64_Count) {
  std::vector<int64_t> values = {1, 2, 3, 4, 5};
  EXPECT_EQ(ApplyAggregation(values, TreeAggType::kCount), 5);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Int64_Any) {
  std::vector<int64_t> values = {42, 1, 2, 3};
  EXPECT_EQ(ApplyAggregation(values, TreeAggType::kAny), 42);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Double_Min) {
  std::vector<double> values = {5.5, 2.2, 8.8, 1.1, 9.9};
  EXPECT_DOUBLE_EQ(ApplyAggregation(values, TreeAggType::kMin), 1.1);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Double_Max) {
  std::vector<double> values = {5.5, 2.2, 8.8, 1.1, 9.9};
  EXPECT_DOUBLE_EQ(ApplyAggregation(values, TreeAggType::kMax), 9.9);
}

TEST(TreeAlgorithmsTest, ApplyAggregation_Double_Sum) {
  std::vector<double> values = {1.0, 2.0, 3.0};
  EXPECT_DOUBLE_EQ(ApplyAggregation(values, TreeAggType::kSum), 6.0);
}

// =============================================================================
// MergeSiblings tests
// =============================================================================

TEST(TreeAlgorithmsTest, MergeSiblings_NoMerge) {
  // All siblings have different keys
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
  };
  std::vector<int64_t> keys = {0, 1, 2};  // All different
  std::vector<int64_t> order = {0, 10, 20};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kConsecutive);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // No merging should happen - each node becomes its own output
  EXPECT_EQ(result->merged_sources.size(), 3u);
  EXPECT_THAT(result->merged_sources[0], ElementsAre(0));
  EXPECT_THAT(result->merged_sources[1], ElementsAre(1));
  EXPECT_THAT(result->merged_sources[2], ElementsAre(2));
}

TEST(TreeAlgorithmsTest, MergeSiblings_ConsecutiveMerge) {
  // Siblings 1 and 2 have same key and are consecutive
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      0,            // 3: child of 0
  };
  std::vector<int64_t> keys = {0, 1, 1, 2};  // 1 and 2 have same key
  std::vector<int64_t> order = {0, 10, 20, 30};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kConsecutive);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Node 0 stays alone, nodes 1 and 2 merge, node 3 stays alone
  EXPECT_EQ(result->merged_sources.size(), 3u);

  // Find the merged group containing 1 and 2
  bool found_merged = false;
  for (auto sources : result->merged_sources) {
    if (sources.size() == 2) {
      EXPECT_THAT(sources, ElementsAre(1, 2));
      found_merged = true;
    }
  }
  EXPECT_TRUE(found_merged);
}

TEST(TreeAlgorithmsTest, MergeSiblings_ConsecutiveNoMergeNonAdjacent) {
  // Siblings 1 and 3 have same key but are NOT consecutive (2 is between)
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      0,            // 3: child of 0
  };
  std::vector<int64_t> keys = {0, 1, 2, 1};  // 1 and 3 have same key
  std::vector<int64_t> order = {0, 10, 20, 30};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kConsecutive);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // With CONSECUTIVE mode, 1 and 3 should NOT merge (2 is between them)
  EXPECT_EQ(result->merged_sources.size(), 4u);
  for (auto sources : result->merged_sources) {
    EXPECT_EQ(sources.size(), 1u);
  }
}

TEST(TreeAlgorithmsTest, MergeSiblings_GlobalMergeNonAdjacent) {
  // Siblings 1 and 3 have same key, should merge in GLOBAL mode
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      0,            // 3: child of 0
  };
  std::vector<int64_t> keys = {0, 1, 2, 1};  // 1 and 3 have same key
  std::vector<int64_t> order = {0, 10, 20, 30};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kGlobal);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // With GLOBAL mode, 1 and 3 SHOULD merge
  EXPECT_EQ(result->merged_sources.size(), 3u);

  // Find the merged group containing 1 and 3
  bool found_merged = false;
  for (auto sources : result->merged_sources) {
    if (sources.size() == 2) {
      // Should contain both 1 and 3
      EXPECT_TRUE((sources[0] == 1 && sources[1] == 3) ||
                  (sources[0] == 3 && sources[1] == 1));
      found_merged = true;
    }
  }
  EXPECT_TRUE(found_merged);
}

TEST(TreeAlgorithmsTest, MergeSiblings_StringPoolIdKeys) {
  StringPool pool;
  StringPool::Id root_id = pool.InternString(base::StringView("root"));
  StringPool::Id a_id = pool.InternString(base::StringView("a"));
  StringPool::Id b_id = pool.InternString(base::StringView("b"));

  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      0,            // 3: child of 0
  };
  std::vector<StringPool::Id> keys = {root_id, a_id, a_id, b_id};
  std::vector<int64_t> order = {0, 10, 20, 30};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kConsecutive);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Nodes 1 and 2 (key "a") should merge
  EXPECT_EQ(result->merged_sources.size(), 3u);

  bool found_merged = false;
  for (auto sources : result->merged_sources) {
    if (sources.size() == 2) {
      EXPECT_THAT(sources, ElementsAre(1, 2));
      found_merged = true;
    }
  }
  EXPECT_TRUE(found_merged);
}

TEST(TreeAlgorithmsTest, MergeSiblings_PreservesParentRelationship) {
  // After merging, parent relationships should be updated correctly
  std::vector<uint32_t> parent_indices = {
      kNullUint32,  // 0: root
      0,            // 1: child of 0
      0,            // 2: child of 0
      1,            // 3: child of 1
      1,            // 4: child of 1
  };
  std::vector<int64_t> keys = {0, 1, 1, 2, 2};  // 1,2 merge; 3,4 merge
  std::vector<int64_t> order = {0, 10, 20, 100, 200};

  auto result =
      MergeSiblings(parent_indices, keys, order, TreeMergeMode::kConsecutive);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // After merging:
  // - Root (0) stays as is
  // - Nodes 1,2 merge into one node
  // - Nodes 3,4 merge into one node, whose parent is the merged 1,2 node

  // Find the new index for merged(1,2)
  uint32_t merged_12_idx = kNullUint32;
  for (uint32_t i = 0; i < result->merged_sources.size(); ++i) {
    const auto& sources = result->merged_sources[i];
    if (sources.size() == 2 && ((sources[0] == 1 && sources[1] == 2) ||
                                (sources[0] == 2 && sources[1] == 1))) {
      merged_12_idx = i;
      break;
    }
  }
  ASSERT_NE(merged_12_idx, kNullUint32);

  // Find the new index for merged(3,4) and verify its parent
  for (uint32_t i = 0; i < result->merged_sources.size(); ++i) {
    const auto& sources = result->merged_sources[i];
    if (sources.size() == 2 && ((sources[0] == 3 && sources[1] == 4) ||
                                (sources[0] == 4 && sources[1] == 3))) {
      // Parent should be merged_12_idx
      EXPECT_EQ(result->new_parent_indices[i], merged_12_idx);
    }
  }
}

// =============================================================================
// DeleteNodes tests
// =============================================================================

// Helper to create a TreeData with int64 passthrough column
TreeData CreateTreeDataWithInt64Column(
    const std::vector<uint32_t>& parent_indices,
    const std::string& col_name,
    const std::vector<int64_t>& col_values) {
  TreeData data;
  data.parent_indices = parent_indices;

  PassthroughColumn col;
  col.name = col_name;
  col.data = col_values;
  data.passthrough_columns.push_back(std::move(col));

  return data;
}

// Helper to create a TreeData with string passthrough column
TreeData CreateTreeDataWithStringColumn(
    const std::vector<uint32_t>& parent_indices,
    const std::string& col_name,
    const std::vector<StringPool::Id>& col_values) {
  TreeData data;
  data.parent_indices = parent_indices;

  PassthroughColumn col;
  col.name = col_name;
  col.data = col_values;
  data.passthrough_columns.push_back(std::move(col));

  return data;
}

TEST(TreeAlgorithmsTest, DeleteNodes_Empty) {
  TreeData data;
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{42});

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->new_parent_indices, IsEmpty());
  EXPECT_THAT(result->old_to_new, IsEmpty());
}

TEST(TreeAlgorithmsTest, DeleteNodes_NoMatch) {
  // Tree: 0 -> 1 -> 2
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{42});  // No match

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // All nodes survive
  EXPECT_EQ(result->new_parent_indices.size(), 3u);
  EXPECT_THAT(result->old_to_new, ElementsAre(0, 1, 2));
}

TEST(TreeAlgorithmsTest, DeleteNodes_DeleteLeaf) {
  // Tree: 0 -> 1 -> 2 (delete node 2)
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{3});

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Two nodes survive (0 and 1)
  EXPECT_EQ(result->new_parent_indices.size(), 2u);
  EXPECT_EQ(result->old_to_new[0], 0u);           // Node 0 -> new 0
  EXPECT_EQ(result->old_to_new[1], 1u);           // Node 1 -> new 1
  EXPECT_EQ(result->old_to_new[2], kNullUint32);  // Node 2 deleted

  // New parent indices: node 0 is root, node 1's parent is still 0
  EXPECT_EQ(result->new_parent_indices[0], kNullUint32);
  EXPECT_EQ(result->new_parent_indices[1], 0u);
}

TEST(TreeAlgorithmsTest, DeleteNodes_DeleteInternal_ReparentChildren) {
  // Tree:
  //   0 (root, val=1)
  //   └── 1 (val=2) <- DELETE THIS
  //       └── 2 (val=3)
  // After delete: 0 -> 2
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq,
                      int64_t{2});  // Delete node with val=2 (internal node)

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Two nodes survive (0 and 2)
  EXPECT_EQ(result->new_parent_indices.size(), 2u);
  EXPECT_EQ(result->old_to_new[0], 0u);           // Node 0 -> new 0
  EXPECT_EQ(result->old_to_new[1], kNullUint32);  // Node 1 deleted
  EXPECT_EQ(result->old_to_new[2], 1u);           // Node 2 -> new 1

  // Node 2 is now reparented to node 0 (skipping deleted node 1)
  EXPECT_EQ(result->new_parent_indices[0], kNullUint32);  // Root
  EXPECT_EQ(result->new_parent_indices[1],
            0u);  // Former child of 1, now child of 0
}

TEST(TreeAlgorithmsTest, DeleteNodes_DeleteRoot_ChildBecomesRoot) {
  // Tree:
  //   0 (root, val=1) <- DELETE THIS
  //   └── 1 (val=2)
  //       └── 2 (val=3)
  // After delete: 1 (now root) -> 2
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{1});  // Delete root

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Two nodes survive (1 and 2)
  EXPECT_EQ(result->new_parent_indices.size(), 2u);
  EXPECT_EQ(result->old_to_new[0], kNullUint32);  // Node 0 deleted
  EXPECT_EQ(result->old_to_new[1], 0u);           // Node 1 -> new 0
  EXPECT_EQ(result->old_to_new[2], 1u);           // Node 2 -> new 1

  // Node 1 is now root, node 2's parent is node 1
  EXPECT_EQ(result->new_parent_indices[0], kNullUint32);  // New root
  EXPECT_EQ(result->new_parent_indices[1], 0u);  // Parent is new node 0
}

TEST(TreeAlgorithmsTest, DeleteNodes_DeleteAll) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val",
                                            {1, 1, 1});  // All have same value
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{1});

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // All deleted
  EXPECT_THAT(result->new_parent_indices, IsEmpty());
  EXPECT_THAT(result->old_to_new,
              ElementsAre(kNullUint32, kNullUint32, kNullUint32));
}

TEST(TreeAlgorithmsTest, DeleteNodes_StringEq) {
  StringPool pool;
  StringPool::Id idle = pool.InternString(base::StringView("idle"));
  StringPool::Id work = pool.InternString(base::StringView("work"));
  StringPool::Id sleep = pool.InternString(base::StringView("sleep"));

  // Tree: 0 -> 1 -> 2
  auto data = CreateTreeDataWithStringColumn(
      {kNullUint32, 0, 1}, "name", {work, idle, sleep});  // Delete "idle"

  TreeDeleteSpec spec("name", TreeCompareOp::kEq, idle);

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Node 1 deleted, node 2 reparented to node 0
  EXPECT_EQ(result->new_parent_indices.size(), 2u);
  EXPECT_EQ(result->old_to_new[0], 0u);
  EXPECT_EQ(result->old_to_new[1], kNullUint32);
  EXPECT_EQ(result->old_to_new[2], 1u);
  EXPECT_EQ(result->new_parent_indices[1], 0u);  // 2 -> 0
}

TEST(TreeAlgorithmsTest, DeleteNodes_GlobMatch) {
  StringPool pool;
  StringPool::Id foo_bar = pool.InternString(base::StringView("foo_bar"));
  StringPool::Id foo_baz = pool.InternString(base::StringView("foo_baz"));
  StringPool::Id other = pool.InternString(base::StringView("other"));

  // Tree: 0 -> 1, 0 -> 2
  auto data = CreateTreeDataWithStringColumn({kNullUint32, 0, 0}, "name",
                                             {other, foo_bar, foo_baz});

  StringPool::Id pattern = pool.InternString(base::StringView("foo_*"));

  TreeDeleteSpec spec("name", TreeCompareOp::kGlob, pattern);

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Nodes 1 and 2 deleted (match foo_*)
  EXPECT_EQ(result->new_parent_indices.size(), 1u);
  EXPECT_EQ(result->old_to_new[0], 0u);
  EXPECT_EQ(result->old_to_new[1], kNullUint32);
  EXPECT_EQ(result->old_to_new[2], kNullUint32);
}

TEST(TreeAlgorithmsTest, DeleteNodes_MultipleInternalDeletes) {
  // Complex tree:
  //   0 (root)
  //   ├── 1 <- DELETE
  //   │   └── 3
  //   └── 2 <- DELETE
  //       └── 4
  // After: 0 -> 3, 0 -> 4
  auto data = CreateTreeDataWithInt64Column(
      {kNullUint32, 0, 0, 1, 2}, "val",
      {0, 1, 1, 0, 0});  // Delete nodes with val=1 (nodes 1 and 2)
  StringPool pool;

  TreeDeleteSpec spec("val", TreeCompareOp::kEq, int64_t{1});

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Nodes 0, 3, 4 survive
  EXPECT_EQ(result->new_parent_indices.size(), 3u);
  EXPECT_EQ(result->old_to_new[0], 0u);
  EXPECT_EQ(result->old_to_new[1], kNullUint32);
  EXPECT_EQ(result->old_to_new[2], kNullUint32);
  EXPECT_EQ(result->old_to_new[3], 1u);
  EXPECT_EQ(result->old_to_new[4], 2u);

  // 3 and 4 are now direct children of 0
  EXPECT_EQ(result->new_parent_indices[0], kNullUint32);  // Root
  EXPECT_EQ(result->new_parent_indices[1], 0u);           // 3 -> 0
  EXPECT_EQ(result->new_parent_indices[2], 0u);           // 4 -> 0
}

TEST(TreeAlgorithmsTest, DeleteNodes_ColumnNotFound) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32}, "val", {1});
  StringPool pool;

  TreeDeleteSpec spec("nonexistent", TreeCompareOp::kEq, int64_t{1});

  auto result = DeleteNodes(data, spec, &pool);
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().c_message(), testing::HasSubstr("not found"));
}

// =============================================================================
// PropagateUp tests
// =============================================================================

TEST(TreeAlgorithmsTest, PropagateUp_Empty) {
  TreeData data;
  TreePropagateSpec spec("out", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_EQ(result->out_column.name, "out");
}

TEST(TreeAlgorithmsTest, PropagateUp_SingleNode) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32}, "val", {42});

  TreePropagateSpec spec("cumulative", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(42));
}

TEST(TreeAlgorithmsTest, PropagateUp_Chain_Sum) {
  // Tree: 0 -> 1 -> 2
  // vals:  1    2    3
  // Expected cumulative (SUM): 6, 5, 3
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});

  TreePropagateSpec spec("cumulative", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 2 (leaf): 3
  // Node 1: 2 + 3 = 5
  // Node 0: 1 + 5 = 6
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(6, 5, 3));
}

TEST(TreeAlgorithmsTest, PropagateUp_BranchingTree_Sum) {
  // Tree:
  //   0 (val=1)
  //   ├── 1 (val=2)
  //   └── 2 (val=3)
  //       └── 3 (val=4)
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, 0, 2}, "val",
                                            {1, 2, 3, 4});

  TreePropagateSpec spec("cumulative", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 3 (leaf): 4
  // Node 2: 3 + 4 = 7
  // Node 1 (leaf): 2
  // Node 0: 1 + 2 + 7 = 10
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(10, 2, 7, 4));
}

TEST(TreeAlgorithmsTest, PropagateUp_Max) {
  // Tree: 0 -> 1 -> 2
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {5, 10, 3});

  TreePropagateSpec spec("max_val", "val", TreeAggType::kMax);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 2: max(3) = 3
  // Node 1: max(10, 3) = 10
  // Node 0: max(5, 10) = 10
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(10, 10, 3));
}

TEST(TreeAlgorithmsTest, PropagateUp_Min) {
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {5, 10, 3});

  TreePropagateSpec spec("min_val", "val", TreeAggType::kMin);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 2: min(3) = 3
  // Node 1: min(10, 3) = 3
  // Node 0: min(5, 3) = 3
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(3, 3, 3));
}

TEST(TreeAlgorithmsTest, PropagateUp_Double) {
  TreeData data;
  data.parent_indices = {kNullUint32, 0, 1};
  PassthroughColumn col;
  col.name = "val";
  col.data = std::vector<double>{1.5, 2.5, 3.5};
  data.passthrough_columns.push_back(std::move(col));

  TreePropagateSpec spec("cumulative", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  const auto& out = result->out_column.AsDouble();
  EXPECT_DOUBLE_EQ(out[0], 7.5);  // 1.5 + 2.5 + 3.5
  EXPECT_DOUBLE_EQ(out[1], 6.0);  // 2.5 + 3.5
  EXPECT_DOUBLE_EQ(out[2], 3.5);
}

TEST(TreeAlgorithmsTest, PropagateUp_ColumnNotFound) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32}, "val", {42});

  TreePropagateSpec spec("out", "nonexistent", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().c_message(), testing::HasSubstr("not found"));
}

TEST(TreeAlgorithmsTest, PropagateUp_MultipleRoots) {
  // Two separate trees: 0->1 and 2->3
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, kNullUint32, 2},
                                            "val", {1, 2, 10, 20});

  TreePropagateSpec spec("cumulative", "val", TreeAggType::kSum);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Tree 1: node 1=2, node 0=1+2=3
  // Tree 2: node 3=20, node 2=10+20=30
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(3, 2, 30, 20));
}

TEST(TreeAlgorithmsTest, PropagateUp_Count) {
  // Tree:
  //   0 (root)
  //   ├── 1
  //   └── 2
  //       └── 3
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, 0, 2}, "val",
                                            {1, 1, 1, 1});

  TreePropagateSpec spec("agg_count", "val", TreeAggType::kCount);

  auto result = PropagateUp(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // COUNT returns the number of values being aggregated:
  // Node 3 (leaf): count([val_3]) = 1
  // Node 1 (leaf): count([val_1]) = 1
  // Node 2: count([val_2, out_3]) = count([1, 1]) = 2
  // Node 0: count([val_0, out_1, out_2]) = count([1, 1, 2]) = 3
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(3, 1, 2, 1));
}

// =============================================================================
// PropagateDown tests
// =============================================================================

TEST(TreeAlgorithmsTest, PropagateDown_Empty) {
  TreeData data;
  TreePropagateSpec spec("out", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_EQ(result->out_column.name, "out");
}

TEST(TreeAlgorithmsTest, PropagateDown_SingleNode) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32}, "val", {42});

  TreePropagateSpec spec("path_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Single root node: output = input
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(42));
}

TEST(TreeAlgorithmsTest, PropagateDown_Chain_Sum) {
  // Tree: 0 -> 1 -> 2
  // vals:  1    2    3
  // Expected path_sum (SUM from root): 1, 3, 6
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {1, 2, 3});

  TreePropagateSpec spec("path_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 0 (root): 1
  // Node 1: 1 + 2 = 3
  // Node 2: 3 + 3 = 6
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(1, 3, 6));
}

TEST(TreeAlgorithmsTest, PropagateDown_BranchingTree_Sum) {
  // Tree:
  //   0 (val=1)
  //   ├── 1 (val=2)
  //   └── 2 (val=3)
  //       └── 3 (val=4)
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, 0, 2}, "val",
                                            {1, 2, 3, 4});

  TreePropagateSpec spec("path_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 0: 1
  // Node 1: 1 + 2 = 3
  // Node 2: 1 + 3 = 4
  // Node 3: 4 + 4 = 8
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(1, 3, 4, 8));
}

TEST(TreeAlgorithmsTest, PropagateDown_Max) {
  // Tree: 0 -> 1 -> 2
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {5, 10, 3});

  TreePropagateSpec spec("max_path", "val", TreeAggType::kMax);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 0: max(5) = 5
  // Node 1: max(5, 10) = 10
  // Node 2: max(10, 3) = 10
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(5, 10, 10));
}

TEST(TreeAlgorithmsTest, PropagateDown_Min) {
  auto data =
      CreateTreeDataWithInt64Column({kNullUint32, 0, 1}, "val", {5, 10, 3});

  TreePropagateSpec spec("min_path", "val", TreeAggType::kMin);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Node 0: min(5) = 5
  // Node 1: min(5, 10) = 5
  // Node 2: min(5, 3) = 3
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(5, 5, 3));
}

TEST(TreeAlgorithmsTest, PropagateDown_Double) {
  TreeData data;
  data.parent_indices = {kNullUint32, 0, 1};
  PassthroughColumn col;
  col.name = "val";
  col.data = std::vector<double>{1.5, 2.5, 3.5};
  data.passthrough_columns.push_back(std::move(col));

  TreePropagateSpec spec("path_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  const auto& out = result->out_column.AsDouble();
  EXPECT_DOUBLE_EQ(out[0], 1.5);  // Root
  EXPECT_DOUBLE_EQ(out[1], 4.0);  // 1.5 + 2.5
  EXPECT_DOUBLE_EQ(out[2], 7.5);  // 4.0 + 3.5
}

TEST(TreeAlgorithmsTest, PropagateDown_ColumnNotFound) {
  auto data = CreateTreeDataWithInt64Column({kNullUint32}, "val", {42});

  TreePropagateSpec spec("out", "nonexistent", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().c_message(), testing::HasSubstr("not found"));
}

TEST(TreeAlgorithmsTest, PropagateDown_MultipleRoots) {
  // Two separate trees: 0->1 and 2->3
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, kNullUint32, 2},
                                            "val", {1, 2, 10, 20});

  TreePropagateSpec spec("path_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Tree 1: node 0=1, node 1=1+2=3
  // Tree 2: node 2=10, node 3=10+20=30
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(1, 3, 10, 30));
}

TEST(TreeAlgorithmsTest, PropagateDown_DeepChain) {
  // Chain: 0 -> 1 -> 2 -> 3 -> 4
  auto data = CreateTreeDataWithInt64Column({kNullUint32, 0, 1, 2, 3}, "val",
                                            {1, 1, 1, 1, 1});

  TreePropagateSpec spec("depth_sum", "val", TreeAggType::kSum);

  auto result = PropagateDown(data, spec);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  // Each node accumulates sum from root
  EXPECT_THAT(result->out_column.AsInt64(), ElementsAre(1, 2, 3, 4, 5));
}

// =============================================================================
// InvertAndMerge tests
// =============================================================================

TEST(TreeAlgorithmsTest, InvertAndMerge_Empty) {
  std::vector<uint32_t> parent_indices;
  std::vector<int64_t> keys;
  std::vector<int64_t> order;

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();
  EXPECT_THAT(result->merged_sources, IsEmpty());
  EXPECT_THAT(result->new_parent_indices, IsEmpty());
  EXPECT_THAT(result->old_to_new, IsEmpty());
}

TEST(TreeAlgorithmsTest, InvertAndMerge_SingleNode) {
  // Single node is both a root and a leaf
  std::vector<uint32_t> parent_indices = {kNullUint32};
  std::vector<int64_t> keys = {42};
  std::vector<int64_t> order = {0};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Single node becomes a root in inverted tree
  EXPECT_EQ(result->merged_sources.size(), 1u);
  EXPECT_THAT(result->merged_sources[0], ElementsAre(0));
  EXPECT_EQ(result->new_parent_indices[0], kNullUint32);
  EXPECT_EQ(result->old_to_new[0], 0u);
}

TEST(TreeAlgorithmsTest, InvertAndMerge_SimpleChain) {
  // Original: 0 -> 1 -> 2 (0 is root, 2 is leaf)
  // Inverted: 2 -> 1 -> 0 (2 is root, 0 is leaf)
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 1};
  std::vector<int64_t> keys = {1, 2, 3};  // All different, no merge
  std::vector<int64_t> order = {0, 10, 20};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // All 3 nodes survive, no merging
  EXPECT_EQ(result->merged_sources.size(), 3u);

  // Find which output is the new root (original leaf, node 2)
  uint32_t new_root_idx = kNullUint32;
  for (uint32_t i = 0; i < result->new_parent_indices.size(); ++i) {
    if (result->new_parent_indices[i] == kNullUint32) {
      // This should be from source node 2
      EXPECT_THAT(result->merged_sources[i], ElementsAre(2));
      new_root_idx = i;
      break;
    }
  }
  ASSERT_NE(new_root_idx, kNullUint32);

  // The original root (node 0) should be a leaf in inverted tree
  // Its parent should be from original node 1
  uint32_t orig_root_new_idx = result->old_to_new[0];
  EXPECT_NE(result->new_parent_indices[orig_root_new_idx], kNullUint32);
}

TEST(TreeAlgorithmsTest, InvertAndMerge_BranchingTree_DuplicatesPath) {
  // Original tree:
  //   0 (root, key=A)
  //   ├── 1 (key=B)
  //   └── 2 (key=C)
  // Leaves: 1 and 2
  // Inverted paths:
  //   Path from 1: 1(B) -> 0(A)
  //   Path from 2: 2(C) -> 0(A)
  // Node 0 appears in two paths, so it becomes two separate nodes
  // (merged if same key+path, but here the paths are different)
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 0};
  std::vector<int64_t> keys = {1, 2, 3};  // All different
  std::vector<int64_t> order = {0, 10, 20};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // We have 2 leaves (1 and 2) which become roots
  // Node 0 is duplicated for each path
  // Total: 4 nodes (1, 0_via_1, 2, 0_via_2)
  EXPECT_EQ(result->merged_sources.size(), 4u);

  // Count roots (original leaves)
  uint32_t root_count = 0;
  for (uint32_t idx : result->new_parent_indices) {
    if (idx == kNullUint32) {
      ++root_count;
    }
  }
  EXPECT_EQ(root_count, 2u);  // Two leaves became two roots
}

TEST(TreeAlgorithmsTest, InvertAndMerge_MergesByKey) {
  // Original tree:
  //   0 (root, key=A)
  //   ├── 1 (key=B)
  //   └── 2 (key=B)  <- Same key as 1!
  // Inverted paths:
  //   Path from 1: 1(B) -> 0(A)
  //   Path from 2: 2(B) -> 0(A)
  // Both paths have same structure (root B, child A), so they merge!
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 0};
  std::vector<int64_t> keys = {1, 2, 2};  // 1 and 2 have same key
  std::vector<int64_t> order = {0, 10, 20};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Nodes 1 and 2 have the same key, and their parents (both node 0)
  // also have the same key. So:
  // - The two B-keyed roots merge: sources [1, 2]
  // - Their children (both A-keyed from node 0) merge: sources [0, 0] -> [0]
  // Result: 2 nodes total
  EXPECT_EQ(result->merged_sources.size(), 2u);

  // Find the merged root (should have sources [1, 2])
  bool found_merged_root = false;
  for (uint32_t i = 0; i < result->merged_sources.size(); ++i) {
    if (result->new_parent_indices[i] == kNullUint32) {
      // This is a root - should be merged from 1 and 2
      EXPECT_EQ(result->merged_sources[i].size(), 2u);
      found_merged_root = true;
    }
  }
  EXPECT_TRUE(found_merged_root);
}

TEST(TreeAlgorithmsTest, InvertAndMerge_StringKeys) {
  StringPool pool;
  StringPool::Id a = pool.InternString(base::StringView("a"));
  StringPool::Id b = pool.InternString(base::StringView("b"));
  StringPool::Id c = pool.InternString(base::StringView("c"));

  // Chain with string keys
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 1};
  std::vector<StringPool::Id> keys = {a, b, c};
  std::vector<int64_t> order = {0, 10, 20};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // All different keys, no merging
  EXPECT_EQ(result->merged_sources.size(), 3u);
}

TEST(TreeAlgorithmsTest, InvertAndMerge_DiamondTree) {
  // Original tree (diamond):
  //       0 (root, key=A)
  //      / \
  //     1   2   (key=B, key=C)
  //      \ /
  //       3     (key=D, leaf)
  // But wait - this is a DAG, not a tree! parent_indices only allows one
  // parent. Let's do a simpler case:
  //       0 (key=A)
  //      / \
  //     1   2 (key=B, key=B - same!)
  //     |   |
  //     3   4 (key=C, key=C - same!)
  // Inverted paths:
  //   3 -> 1 -> 0   =>  C -> B -> A
  //   4 -> 2 -> 0   =>  C -> B -> A
  // Both paths are identical (C->B->A), so everything merges!
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 0, 1, 2};
  std::vector<int64_t> keys = {1, 2, 2, 3, 3};  // A=1, B=2, B=2, C=3, C=3
  std::vector<int64_t> order = {0, 10, 20, 100, 200};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Both paths C->B->A merge into a single chain of 3 nodes
  EXPECT_EQ(result->merged_sources.size(), 3u);

  // Verify we have exactly one root
  uint32_t root_count = 0;
  for (uint32_t idx : result->new_parent_indices) {
    if (idx == kNullUint32) {
      ++root_count;
    }
  }
  EXPECT_EQ(root_count, 1u);
}

TEST(TreeAlgorithmsTest, InvertAndMerge_PartialMerge) {
  // Original:
  //       0 (key=A)
  //      / \
  //     1   2 (key=B, key=C)  <- Different keys!
  //     |   |
  //     3   4 (key=D, key=D)  <- Same keys
  // Inverted paths:
  //   3 -> 1 -> 0   =>  D -> B -> A
  //   4 -> 2 -> 0   =>  D -> C -> A
  // At root level: both D nodes have (inverted_parent=null, key=D) -> MERGE
  // At level 1: B and C have different keys -> NO merge
  // At level 2: A nodes have different inverted_parents (B vs C) -> NO merge
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 0, 1, 2};
  std::vector<int64_t> keys = {1, 2, 3, 4, 4};  // A, B, C, D, D
  std::vector<int64_t> order = {0, 10, 20, 100, 200};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Result:
  //   merged_D (sources: 3, 4) - root
  //     ├── B (source: 1)
  //     │   └── A_via_B (source: 0)
  //     └── C (source: 2)
  //         └── A_via_C (source: 0)
  // That's 5 nodes
  EXPECT_EQ(result->merged_sources.size(), 5u);

  // 1 root (merged D node)
  uint32_t root_count = 0;
  for (uint32_t idx : result->new_parent_indices) {
    if (idx == kNullUint32) {
      ++root_count;
    }
  }
  EXPECT_EQ(root_count, 1u);

  // Find the root node and verify it merged 3 and 4
  for (uint32_t i = 0; i < result->merged_sources.size(); ++i) {
    if (result->new_parent_indices[i] == kNullUint32) {
      EXPECT_THAT(result->merged_sources[i], ElementsAre(3, 4));
    }
  }
}

TEST(TreeAlgorithmsTest, InvertAndMerge_OldToNewMapping) {
  // Verify old_to_new correctly maps original nodes to output nodes
  std::vector<uint32_t> parent_indices = {kNullUint32, 0, 1};
  std::vector<int64_t> keys = {1, 2, 3};
  std::vector<int64_t> order = {0, 10, 20};

  auto result = InvertAndMerge(parent_indices, keys, order);
  ASSERT_TRUE(result.ok()) << result.status().c_message();

  // Each original node should map to some output node
  EXPECT_EQ(result->old_to_new.size(), 3u);
  for (uint32_t i = 0; i < 3; ++i) {
    EXPECT_NE(result->old_to_new[i], kNullUint32);
    EXPECT_LT(result->old_to_new[i], result->merged_sources.size());
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::plugins::tree
