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

#include "src/trace_processor/core/exec/group_table.h"

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// The groups of a batch of `count` rows of `columns`.
std::vector<uint32_t> Groups(GroupTable& table,
                             std::vector<ColumnView> columns,
                             uint32_t count) {
  RowBatch batch;
  std::vector<uint32_t> keys;
  for (ColumnView& column : columns) {
    keys.push_back(batch.column_count());
    batch.AddBorrowedColumn(column);
  }
  batch.SetRowCount(count);
  std::vector<uint32_t> groups(count);
  EXPECT_EQ(table.FindOrCreate(batch, keys, groups.data()), std::nullopt);
  return groups;
}

TEST(GroupTableTest, NumbersKeysInTheOrderFirstSeen) {
  GroupTable table;
  std::vector<int64_t> keys = {7, 3, 7, 9, 3};
  EXPECT_THAT(
      Groups(table, {ColumnView::Reference(StorageType{Int64{}}, keys.data())},
             5),
      ElementsAre(0, 1, 0, 2, 1));
  // Later batches find the groups of earlier ones.
  std::vector<int64_t> more = {9, 11};
  EXPECT_THAT(
      Groups(table, {ColumnView::Reference(StorageType{Int64{}}, more.data())},
             2),
      ElementsAre(2, 3));
  EXPECT_EQ(table.size(), 4u);
}

// Integers agree whatever their width; nulls agree with each other but not
// with zero; doubles are their bits, so -0 is not 0; strings are their ids;
// and a key of several columns needs all of them to agree.
TEST(GroupTableTest, WhichKeysAgree) {
  {
    GroupTable table;
    std::vector<int64_t> wide = {5, 6};
    std::vector<uint32_t> narrow = {6, 5};
    Groups(table, {ColumnView::Reference(StorageType{Int64{}}, wide.data())},
           2);
    EXPECT_THAT(
        Groups(table,
               {ColumnView::Reference(StorageType{Uint32{}}, narrow.data())},
               2),
        ElementsAre(1, 0));
  }
  {
    GroupTable table;
    std::vector<int64_t> keys = {0, 0, 0, 4};
    BitVector valid = BitVector::CreateWithSize(4, true);
    valid.clear(1);
    valid.clear(2);
    EXPECT_THAT(Groups(table,
                       {ColumnView::Reference(StorageType{Int64{}}, keys.data(),
                                              &valid)},
                       4),
                ElementsAre(0, 1, 1, 2));
  }
  {
    GroupTable table;
    std::vector<double> keys = {1.5, -0.0, 0.0, 1.5};
    EXPECT_THAT(
        Groups(table,
               {ColumnView::Reference(StorageType{Double{}}, keys.data())}, 4),
        ElementsAre(0, 1, 2, 0));
  }
  {
    GroupTable table;
    std::vector<StringPool::Id> keys = {
        StringPool::Id::Raw(4), StringPool::Id::Raw(2), StringPool::Id::Raw(4)};
    EXPECT_THAT(
        Groups(table,
               {ColumnView::Reference(StorageType{String{}}, keys.data())}, 3),
        ElementsAre(0, 1, 0));
  }
  {
    GroupTable table;
    std::vector<int64_t> a = {1, 1, 2, 1};
    std::vector<int64_t> b = {1, 2, 1, 1};
    EXPECT_THAT(Groups(table,
                       {ColumnView::Reference(StorageType{Int64{}}, a.data()),
                        ColumnView::Reference(StorageType{Int64{}}, b.data())},
                       4),
                ElementsAre(0, 1, 2, 0));
  }
}

TEST(GroupTableTest, GrowsPastManyGroups) {
  GroupTable table;
  std::vector<int64_t> keys(2048);
  for (uint32_t batch = 0; batch < 16; ++batch) {
    for (uint32_t i = 0; i < 2048; ++i) {
      keys[i] = int64_t{batch * 2048 + i} * 7919;
    }
    std::vector<uint32_t> groups = Groups(
        table, {ColumnView::Reference(StorageType{Int64{}}, keys.data())},
        2048);
    for (uint32_t i = 0; i < 2048; ++i) {
      ASSERT_EQ(groups[i], batch * 2048 + i);
    }
  }
  // Every key is still found after growing.
  std::vector<int64_t> first = {0, 7919};
  EXPECT_THAT(
      Groups(table, {ColumnView::Reference(StorageType{Int64{}}, first.data())},
             2),
      ElementsAre(0, 1));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
