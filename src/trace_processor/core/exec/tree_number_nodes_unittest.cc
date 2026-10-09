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

#include "src/trace_processor/core/exec/tree_number_nodes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

// Runs one batch of ids and parent ids, of any type, through the operator.
template <typename T>
struct Numbered {
  Numbered(StorageType type,
           std::vector<T> ids,
           std::vector<T> parents,
           std::vector<bool> has_parent)
      : op(0, 1),
        state(op.MakeState(test::TestContext())),
        ids_(std::move(ids)),
        parents_(std::move(parents)) {
    auto count = static_cast<uint32_t>(ids_.size());
    validity_ = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (has_parent[i]) {
        validity_.set(i);
      }
    }
    in.AddBorrowedColumn(ColumnView::Reference(type, ids_.data()));
    in.AddBorrowedColumn(
        ColumnView::Reference(type, parents_.data(), &validity_));
    test::Window(&in, 0, count);
  }

  bool Process() { return test::ProcessCopy(op, in, out, *state); }

  TreeNumberNodes op;
  std::unique_ptr<OperatorState> state;
  std::vector<T> ids_;
  std::vector<T> parents_;
  BitVector validity_;
  RowBatch in;
  RowBatch out;
};

// Ids are numbered densely, a parent not yet seen being numbered on sight so
// a child-first stream works. Ids which are already node numbers are left
// alone, without a map.
TEST(TreeNumberNodesTest, IdsAreNumberedDensely) {
  struct Case {
    std::vector<int64_t> ids;
    std::vector<int64_t> parents;
    std::vector<bool> has_parent;
    std::vector<uint32_t> nodes;
    std::vector<uint32_t> parent_nodes;
    bool dense;
  };
  std::vector<Case> cases = {
      {{0, 1, 2},
       {0, 0, 1},
       {false, true, true},
       {0, 1, 2},
       {kNoNode, 0, 1},
       true},
      // Scattered, as a filtered relation's are.
      {{500, 900, 700},
       {0, 500, 900},
       {false, true, true},
       {0, 1, 2},
       {kNoNode, 0, 1},
       false},
      // Child first.
      {{2, 1, 0},
       {1, 0, 0},
       {true, true, false},
       {0, 1, 2},
       {1, 2, kNoNode},
       false},
  };
  for (const Case& c : cases) {
    Numbered<int64_t> run(StorageType{Int64{}}, c.ids, c.parents, c.has_parent);
    ASSERT_TRUE(run.Process());
    EXPECT_EQ(test::ReadColumn<uint32_t>(run.out, 2), c.nodes);
    EXPECT_EQ(test::ReadColumn<uint32_t>(run.out, 3), c.parent_nodes);
    EXPECT_EQ(run.op.IsDenseForTesting(*run.state), c.dense);
  }
}

TEST(TreeNumberNodesTest, VariantStringsAndIntegersHaveSeparateKeys) {
  StringPool pool;
  StringPool::Id string = pool.InternString("id");
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());
  std::vector<Variant> ids = {Variant::Int64(string.raw_id()),
                              Variant::String(string)};
  std::vector<Variant> parents = {Variant::Null(), Variant::Null()};
  RowBatch in;
  in.AddBorrowedColumn(ColumnView::Variants(ids.data()));
  in.AddBorrowedColumn(ColumnView::Variants(parents.data()));
  in.SetRowCount(2);

  RowBatch out;
  ASSERT_TRUE(test::ProcessCopy(op, in, out, *state));
  EXPECT_THAT(test::ReadColumn<uint32_t>(out, 2), ElementsAre(0u, 1u));
}

// Batches of a table's id column and Uint32 parent ids, run through one
// operator in turn.
struct Scanned {
  explicit Scanned(std::vector<uint32_t> parents, std::vector<bool> has_parent)
      : op(0, 1),
        state(op.MakeState(test::TestContext())),
        parents_(std::move(parents)) {
    validity_ =
        BitVector::CreateWithSize(static_cast<uint32_t>(parents_.size()));
    for (uint32_t i = 0; i < parents_.size(); ++i) {
      if (has_parent[i]) {
        validity_.set(i);
      }
    }
  }

  bool Process(uint32_t offset, uint32_t count) {
    RowBatch in;
    in.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
    in.AddBorrowedColumn(ColumnView::Reference(StorageType{Uint32{}},
                                               parents_.data(), &validity_));
    test::Window(&in, offset, count);
    return test::ProcessCopy(op, in, out, *state);
  }

  TreeNumberNodes op;
  std::unique_ptr<OperatorState> state;
  std::vector<uint32_t> parents_;
  BitVector validity_;
  RowBatch out;
};

TEST(TreeNumberNodesTest, DuplicateIdsAreReported) {
  Numbered<int64_t> run(StorageType{Int64{}}, {1, 1}, {0, 0}, {false, false});
  EXPECT_FALSE(run.Process());
  EXPECT_THAT(run.op.status(*run.state).message(),
              testing::HasSubstr("same id"));

  // Rows numbered in order are still known when a later batch repeats one.
  Scanned scanned({0, 0}, {false, true});
  ASSERT_TRUE(scanned.Process(0, 2));
  EXPECT_FALSE(scanned.Process(1, 1));
  EXPECT_THAT(scanned.op.status(*scanned.state).message(),
              testing::HasSubstr("same id"));
}

// A parent pointing at a later row in an otherwise in-order table takes the
// general path, which numbers it identically.
TEST(TreeNumberNodesTest, AParentPointingForwardIsNumberedTheSameWay) {
  Scanned run({0, 2, 0, 1}, {false, true, true, true});
  ASSERT_TRUE(run.Process(0, 4));

  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 2),
              ElementsAre(0u, 1u, 2u, 3u));
  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 3),
              ElementsAre(kNoNode, 2u, 0u, 1u));
  EXPECT_TRUE(run.op.IsDenseForTesting(*run.state));
}

// A parent numbered ahead of its row is not mistaken for a row seen when an
// in-order batch comes between the reference and the row itself.
TEST(TreeNumberNodesTest, AParentNumberedAheadStillGetsItsRow) {
  // Rows 0, 2, 3 in order, then the row with id 1, which row 0 referred to.
  Scanned run({1, 0, 0, 2}, {true, false, true, true});
  ASSERT_TRUE(run.Process(0, 1));
  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 3), ElementsAre(1u));

  ASSERT_TRUE(run.Process(2, 2));
  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 2), ElementsAre(2u, 3u));

  ASSERT_TRUE(run.Process(1, 1));
  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 2), ElementsAre(1u));
  EXPECT_THAT(test::ReadColumn<uint32_t>(run.out, 3), ElementsAre(kNoNode));
  EXPECT_TRUE(run.op.IsDenseForTesting(*run.state));
}

// The shape of a table like stack_profile_callsite: an id column and a
// sparse-null parent id, scanned straight from the dataframe.
inline constexpr auto kCallsiteShaped = dataframe::CreateTypedDataframeSpec(
    {"id", "parent_id"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     NonNull{},
                                     IdSorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{},
                                     SparseNullWithPopcountAlways{},
                                     Unsorted{},
                                     HasDuplicates{}));

// A whole table's ids are already node numbers, so scanning one through the
// operator hands back the row numbers and never builds a map, however many
// batches it takes.
TEST(TreeNumberNodesTest, AScannedIdColumnIsItsOwnNumbering) {
  StringPool pool;
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kCallsiteShaped, &pool);
  constexpr uint32_t kRows = 3 * kMaxBatchRows + 7;
  for (uint32_t row = 0; row < kRows; ++row) {
    std::optional<uint32_t> parent;
    if (row > 0) {
      parent = (row - 1) / 2;
    }
    df.InsertUnchecked(kCallsiteShaped, std::monostate{}, parent);
  }
  df.Finalize();

  DataframeScan scan({df.shared_column(0), df.shared_column(1)},
                     df.row_count());
  std::unique_ptr<OperatorState> scan_state =
      scan.MakeState(test::TestContext());
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState(test::TestContext());

  RowBatch in;
  RowBatch out;
  uint32_t row = 0;
  uint32_t batches = 0;
  while (scan.GetData(in, *scan_state)) {
    ASSERT_TRUE(test::ProcessCopy(op, in, out, *state));
    ASSERT_TRUE(op.IsDenseForTesting(*state));
    std::vector<uint32_t> nodes = test::ReadColumn<uint32_t>(out, 2);
    std::vector<uint32_t> parents = test::ReadColumn<uint32_t>(out, 3);
    ASSERT_EQ(nodes.size(), out.size());
    for (uint32_t i = 0; i < out.size(); ++i, ++row) {
      ASSERT_EQ(nodes[i], row);
      ASSERT_EQ(parents[i], row == 0 ? kNoNode : (row - 1) / 2);
    }
    ++batches;
  }
  EXPECT_EQ(row, kRows);
  EXPECT_EQ(batches, 4u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
