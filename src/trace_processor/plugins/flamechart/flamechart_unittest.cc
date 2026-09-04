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

#include "src/trace_processor/plugins/flamechart/flamechart.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/dataframe_test_utils.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::flamechart {
namespace {

struct Node {
  // Parent row index; nullopt for a root.
  std::optional<uint32_t> parent;
  // Optional original id; defaults to the row index (identity).
  std::optional<int64_t> id;
};

core::Tree MakeTree(const std::vector<Node>& nodes) {
  core::Tree tree;
  tree.row_count = static_cast<uint32_t>(nodes.size());
  tree.parent = core::Slab<uint32_t>::Alloc(nodes.size());
  bool identity = true;
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    tree.parent[i] = nodes[i].parent.value_or(core::Tree::kNullParent);
    identity = identity && nodes[i].id.value_or(i) == i;
  }
  if (!identity) {
    // Mirror BuildTree's tree shape: an Int64 id column plus a populated
    // index over it.
    tree.names = {"id"};
    auto id_col = core::Tree::Column::Create<int64_t>(
        static_cast<uint32_t>(nodes.size()));
    tree.id_index.identity_ids = false;
    tree.id_index.hash.emplace();
    for (uint32_t i = 0; i < nodes.size(); ++i) {
      const int64_t id = nodes[i].id.value_or(i);
      id_col.unchecked_data<int64_t>()[i] = id;
      tree.id_index.hash->Insert(id, i);
    }
    tree.columns.push_back(std::move(id_col));
  }
  return tree;
}

struct Row {
  int64_t ts;
  int64_t dur;
  int64_t depth;
  int64_t id;
  int64_t count;

  bool operator==(const Row& o) const {
    return ts == o.ts && dur == o.dur && depth == o.depth && id == o.id &&
           count == o.count;
  }
};

std::ostream& operator<<(std::ostream& os, const Row& r) {
  return os << "Row{" << r.ts << ", " << r.dur << ", " << r.depth << ", "
            << r.id << ", " << r.count << "}";
}

int64_t AsInt64(const dataframe::ValueVerifier::ValueVariant& v) {
  if (const auto* u32 = std::get_if<uint32_t>(&v)) {
    return *u32;
  }
  if (const auto* i32 = std::get_if<int32_t>(&v)) {
    return *i32;
  }
  if (const auto* i64 = std::get_if<int64_t>(&v)) {
    return *i64;
  }
  PERFETTO_FATAL("Unexpected cell type");
}

std::vector<Row> ReadRuns(dataframe::Dataframe& df) {
  PERFETTO_CHECK(df.column_names().size() == 5);
  std::vector<dataframe::FilterSpec> filters;
  auto plan = df.PlanQuery(filters, {}, {}, {}, 0b11111);
  PERFETTO_CHECK(plan.ok());
  auto cursor =
      std::make_unique<dataframe::Cursor<dataframe::TestRowFetcher>>();
  df.PrepareCursor(std::move(*plan), *cursor);
  dataframe::TestRowFetcher fetcher;
  cursor->Execute(fetcher);

  std::vector<Row> rows;
  for (; !cursor->Eof(); cursor->Next()) {
    dataframe::ValueVerifier verifier;
    verifier.Fetch(&*cursor, 5);
    rows.push_back(Row{AsInt64(verifier.values[0]), AsInt64(verifier.values[1]),
                       AsInt64(verifier.values[2]), AsInt64(verifier.values[3]),
                       AsInt64(verifier.values[4])});
  }
  // Emission order is deterministic but not sorted (mid-sweep closes are
  // deepest-first, final closes root-first); sort for stable expectations.
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.depth != b.depth)
      return a.depth < b.depth;
    if (a.ts != b.ts)
      return a.ts < b.ts;
    return a.id < b.id;
  });
  return rows;
}

class FlamechartRunsTest : public ::testing::Test {
 protected:
  base::StatusOr<dataframe::Dataframe> Build(
      const core::Tree& tree,
      const std::vector<int64_t>& ts,
      const std::vector<int64_t>& leaf_id) {
    return flamechart::Build(tree, core::MakeSpan(ts), core::MakeSpan(leaf_id),
                             &pool_);
  }

  StringPool pool_;
};

// A single run: all points share the full stack, so each depth yields exactly
// one segment spanning the whole range.
TEST_F(FlamechartRunsTest, SingleRunMergesAllPointsPerDepth) {
  // Row 0 = A (root), 1 = B, 2 = C (leaf).
  core::Tree tree = MakeTree({{std::nullopt, {}}, {{0}, {}}, {{1}, {}}});

  auto result = Build(tree, {10, 20, 30}, {2, 2, 2});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0], (Row{10, 20, 0, 0, 3}));
  EXPECT_EQ(rows[1], (Row{10, 20, 1, 1, 3}));
  EXPECT_EQ(rows[2], (Row{10, 20, 2, 2, 3}));
}

// Two runs that share a prefix: the shared depths merge into single segments
// while the divergent leaf depth opens a new segment per run.
TEST_F(FlamechartRunsTest, SharedPrefixMergesAcrossRuns) {
  // 0 = A (root), 1 = B, 2 = C, 3 = D (sibling of C under B).
  core::Tree tree =
      MakeTree({{std::nullopt, {}}, {{0}, {}}, {{1}, {}}, {{1}, {}}});

  auto result = Build(tree, {10, 20, 30, 40}, {2, 2, 3, 3});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 4u);
  // Shared depths (A, B) span the whole range; the leaf depth has one segment
  // per run: C for [10, 30), D for [30, 40].
  EXPECT_EQ(rows[0], (Row{10, 30, 0, 0, 4}));
  EXPECT_EQ(rows[1], (Row{10, 30, 1, 1, 4}));
  EXPECT_EQ(rows[2], (Row{10, 20, 2, 2, 2}));
  EXPECT_EQ(rows[3], (Row{30, 10, 2, 3, 2}));
}

// Two disjoint stacks (different roots): no prefix is shared, every depth
// opens a fresh segment on the leaf change.
TEST_F(FlamechartRunsTest, DisjointStacksOpenFreshSegments) {
  // 0 = A -> 1 = B -> 2 = C; 3 = D -> 4 = E.
  core::Tree tree = MakeTree({{std::nullopt, {}},
                              {{0}, {}},
                              {{1}, {}},
                              {std::nullopt, {}},
                              {{3}, {}}});

  auto result = Build(tree, {10, 20}, {2, 4});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0], (Row{10, 10, 0, 0, 1}));
  EXPECT_EQ(rows[1], (Row{20, 0, 0, 3, 1}));
  EXPECT_EQ(rows[2], (Row{10, 10, 1, 1, 1}));
  EXPECT_EQ(rows[3], (Row{20, 0, 1, 4, 1}));
  EXPECT_EQ(rows[4], (Row{10, 10, 2, 2, 1}));
}

// Depth changes: the new stack extends the shared prefix and adds deeper
// levels, closing the levels that no longer exist.
TEST_F(FlamechartRunsTest, DepthChangeClosesAndOpensTail) {
  // 0 = A (root) -> 1 = B; 0 = A -> 2 = C -> 3 = D.
  core::Tree tree =
      MakeTree({{std::nullopt, {}}, {{0}, {}}, {{0}, {}}, {{2}, {}}});

  auto result = Build(tree, {10, 20}, {1, 3});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0], (Row{10, 10, 0, 0, 2}));
  EXPECT_EQ(rows[1], (Row{10, 10, 1, 1, 1}));
  EXPECT_EQ(rows[2], (Row{20, 0, 1, 2, 1}));
  EXPECT_EQ(rows[3], (Row{20, 0, 2, 3, 1}));
}

// Leaf ids are looked up through the tree's id -> row index, and output runs
// carry the original ids back out.
TEST_F(FlamechartRunsTest, LooksUpOriginalIds) {
  // Row 0 = A (id 100), 1 = B (id 101), 2 = C (id 102).
  core::Tree tree = MakeTree({{std::nullopt, 100}, {{0}, 101}, {{1}, 102}});

  // Points reference the original ids rather than row indices.
  auto result = Build(tree, {10, 20}, {102, 102});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0], (Row{10, 10, 0, 100, 2}));
  EXPECT_EQ(rows[1], (Row{10, 10, 1, 101, 2}));
  EXPECT_EQ(rows[2], (Row{10, 10, 2, 102, 2}));
}

// Points whose leaf cannot be resolved are skipped without breaking open
// segments.
TEST_F(FlamechartRunsTest, SkipsUnresolvableLeaves) {
  core::Tree tree = MakeTree({{std::nullopt, {}}, {{0}, {}}, {{1}, {}}});

  // Second point references a leaf id that does not exist (no id column, and
  // row index 42 is out of range).
  auto result = Build(tree, {10, 15, 20}, {2, 42, 2});
  ASSERT_TRUE(result.ok()) << result.status().message();
  const auto rows = ReadRuns(*result);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0], (Row{10, 10, 0, 0, 2}));
  EXPECT_EQ(rows[1], (Row{10, 10, 1, 1, 2}));
  EXPECT_EQ(rows[2], (Row{10, 10, 2, 2, 2}));
}

// No points produces an empty table with the correct schema.
TEST_F(FlamechartRunsTest, EmptyPointsProduceEmptyOutput) {
  core::Tree tree = MakeTree({{std::nullopt, {}}});

  auto result = Build(tree, {}, {});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->row_count(), 0u);
  EXPECT_EQ(result->column_names().size(), 5u);
}

// Out-of-order timestamps are rejected.
TEST_F(FlamechartRunsTest, RejectsUnsortedTs) {
  core::Tree tree = MakeTree({{std::nullopt, {}}});

  auto result = Build(tree, {20, 10}, {0, 0});
  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::flamechart
