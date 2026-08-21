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
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

std::vector<uint32_t> ReadNodes(const RowBatch& batch, uint32_t column) {
  const ColumnView& view = batch.column(column);
  const auto* data = static_cast<const uint32_t*>(view.data());
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < batch.size(); ++i) {
    out.push_back(data[view.selection().GetIndex(i)]);
  }
  return out;
}

// Runs one batch of ids and parent ids, of any type, through the operator.
template <typename T>
struct Numbered {
  Numbered(StorageType type,
           std::vector<T> ids,
           std::vector<T> parents,
           std::vector<bool> has_parent)
      : op(0, 1),
        state(op.MakeState()),
        ids_(std::move(ids)),
        parents_(std::move(parents)) {
    auto count = static_cast<uint32_t>(ids_.size());
    validity_ = BitVector::CreateWithSize(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (has_parent[i]) {
        validity_.set(i);
      }
    }
    in.AddColumn(ColumnView::Reference(type, ids_.data()));
    in.AddColumn(ColumnView::Reference(type, parents_.data(), &validity_));
    in.Compose(RowSelection::Range(0), count);
    in.SetCardinality(count);
  }

  OpResult Execute() { return op.Execute(in, out, *state); }

  TreeNumberNodes op;
  std::unique_ptr<OperatorState> state;
  std::vector<T> ids_;
  std::vector<T> parents_;
  BitVector validity_;
  RowBatch in;
  RowBatch out;
};

TEST(TreeNumberNodesTest, IdsWhichAreAlreadyNodeNumbersAreLeftAlone) {
  Numbered<int64_t> run(StorageType{Int64{}}, {0, 1, 2}, {0, 0, 1},
                        {false, true, true});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  EXPECT_THAT(ReadNodes(run.out, 2), ElementsAre(0u, 1u, 2u));
  EXPECT_THAT(ReadNodes(run.out, 3), ElementsAre(kNoNode, 0u, 1u));
}

// A filtered relation's ids are scattered over a wide range; numbering them
// makes an array indexed by node the size of the input.
TEST(TreeNumberNodesTest, AScatteringOfIdsIsNumberedDensely) {
  Numbered<int64_t> run(StorageType{Int64{}}, {500, 900, 700}, {0, 500, 900},
                        {false, true, true});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  EXPECT_THAT(ReadNodes(run.out, 2), ElementsAre(0u, 1u, 2u));
  EXPECT_THAT(ReadNodes(run.out, 3), ElementsAre(kNoNode, 0u, 1u));
}

// A parent not yet seen is numbered on sight, so a child-first stream works.
TEST(TreeNumberNodesTest, AParentNotYetSeenIsNumberedAnyway) {
  Numbered<int64_t> run(StorageType{Int64{}}, {2, 1, 0}, {1, 0, 0},
                        {true, true, false});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);

  std::vector<uint32_t> nodes = ReadNodes(run.out, 2);
  std::vector<uint32_t> parents = ReadNodes(run.out, 3);
  EXPECT_EQ(parents[0], nodes[1]);
  EXPECT_EQ(parents[1], nodes[2]);
  EXPECT_EQ(parents[2], kNoNode);
}

TEST(TreeNumberNodesTest, AnIdOfAnyWidthIsNamed) {
  Numbered<uint32_t> run(StorageType{Uint32{}}, {7, 8}, {0, 7}, {false, true});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(run.out, 2), ElementsAre(0u, 1u));
  EXPECT_THAT(ReadNodes(run.out, 3), ElementsAre(kNoNode, 0u));
}

TEST(TreeNumberNodesTest, AStringIsAnIdLikeAnythingElse) {
  StringPool pool;
  StringPool::Id a = pool.InternString("a");
  StringPool::Id b = pool.InternString("b");
  Numbered<StringPool::Id> run(StorageType{String{}}, {a, b}, {a, a},
                               {false, true});
  ASSERT_EQ(run.Execute(), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(run.out, 2), ElementsAre(0u, 1u));
  EXPECT_THAT(ReadNodes(run.out, 3), ElementsAre(kNoNode, 0u));
}

// An Id column has no storage: its value is the row it sits at.
TEST(TreeNumberNodesTest, AnIdColumnIsTheRowItSitsAt) {
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState();
  std::vector<int64_t> parents = {0, 0};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(1);
  RowBatch in;
  in.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
  in.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, parents.data(), &validity));
  in.Compose(RowSelection::Range(0), 2);
  in.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(out, 2), ElementsAre(0u, 1u));
}

TEST(TreeNumberNodesTest, AVariantIdIsNamed) {
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState();
  std::vector<Variant> ids = {Variant::Int64(5), Variant::Int64(9)};
  std::vector<Variant> parents = {Variant::Null(), Variant::Int64(5)};
  RowBatch in;
  in.AddColumn(ColumnView::Variants(ids.data()));
  in.AddColumn(ColumnView::Variants(parents.data()));
  in.Compose(RowSelection::Range(0), 2);
  in.SetCardinality(2);

  RowBatch out;
  ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(out, 2), ElementsAre(0u, 1u));
  EXPECT_THAT(ReadNodes(out, 3), ElementsAre(kNoNode, 0u));
}

TEST(TreeNumberNodesTest, ARowWithNoIdIsReported) {
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState();
  std::vector<int64_t> ids = {1, 2};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(0);
  RowBatch in;
  in.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, ids.data(), &validity));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids.data()));
  in.Compose(RowSelection::Range(0), 2);
  in.SetCardinality(2);

  RowBatch out;
  EXPECT_EQ(op.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(op.status(*state).message(), testing::HasSubstr("no id"));
}

// A node keeps the number it was first given across every later batch.
TEST(TreeNumberNodesTest, NumberingIsStableAcrossBatches) {
  TreeNumberNodes op(0, 1);
  std::unique_ptr<OperatorState> state = op.MakeState();
  std::vector<int64_t> ids = {40, 50, 40};
  RowBatch in;
  RowBatch out;
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids.data()));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids.data()));

  in.Compose(RowSelection::Range(0), 2);
  in.SetCardinality(2);
  ASSERT_EQ(op.Execute(in, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(out, 2), ElementsAre(0u, 1u));

  RowBatch again;
  again.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids.data()));
  again.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ids.data()));
  again.Compose(RowSelection::Range(2), 1);
  again.SetCardinality(1);
  ASSERT_EQ(op.Execute(again, out, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(ReadNodes(out, 2), ElementsAre(0u));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
