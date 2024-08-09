/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <cstdint>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/tables/py_tables_unittest_py.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::tables {

TestEventTable::~TestEventTable() = default;
TestEventChildTable::~TestEventChildTable() = default;
TestSliceTable::~TestSliceTable() = default;
TestArgsTable::~TestArgsTable() = default;

namespace {

class PyTablesUnittest : public ::testing::Test {
 protected:
  StringPool pool_;

  TestEventTable event_{&pool_};
  TestEventChildTable event_child_{&pool_, &event_};
  TestSliceTable slice_{&pool_, &event_};
  TestArgsTable args_{&pool_};
};

TEST_F(PyTablesUnittest, EventTableProprties) {
  ASSERT_STREQ(TestEventTable::Name(), "event");

  ASSERT_EQ(TestEventTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestEventTable::ColumnIndex::type, 1u);
  ASSERT_EQ(TestEventTable::ColumnIndex::ts, 2u);
  ASSERT_EQ(TestEventTable::ColumnIndex::arg_set_id, 3u);

  ASSERT_EQ(TestEventTable::ColumnFlag::ts,
            ColumnLegacy::Flag::kSorted | ColumnLegacy::Flag::kNonNull);
  ASSERT_EQ(TestEventTable::ColumnFlag::arg_set_id,
            ColumnLegacy::Flag::kNonNull);
}

TEST_F(PyTablesUnittest, ArgsTableProprties) {
  ASSERT_STREQ(TestArgsTable::Name(), "args");

  ASSERT_EQ(TestArgsTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestArgsTable::ColumnIndex::type, 1u);
  ASSERT_EQ(TestArgsTable::ColumnIndex::arg_set_id, 2u);

  ASSERT_EQ(TestArgsTable::ColumnFlag::arg_set_id,
            ColumnLegacy::Flag::kSorted | ColumnLegacy::Flag::kSetId |
                ColumnLegacy::Flag::kNonNull);
}

TEST_F(PyTablesUnittest, InsertEvent) {
  event_.Insert(TestEventTable::Row(100, 0));

  ASSERT_EQ(pool_.Get(event_[0].type()).ToStdString(), "event");
  ASSERT_EQ(event_[0].ts(), 100);
  ASSERT_EQ(event_[0].arg_set_id(), 0u);
}

TEST_F(PyTablesUnittest, InsertEventSpecifyCols) {
  TestEventTable::Row row;
  row.ts = 100;
  row.arg_set_id = 0;
  event_.Insert(row);

  ASSERT_EQ(pool_.Get(event_[0].type()).ToStdString(), "event");
  ASSERT_EQ(event_[0].ts(), 100);
  ASSERT_EQ(event_[0].arg_set_id(), 0u);
}

TEST_F(PyTablesUnittest, MutableColumn) {
  event_.Insert(TestEventTable::Row(100, 0));

  ASSERT_EQ(event_[0].ts(), 100);
  ASSERT_EQ(event_[0].arg_set_id(), 0u);
}

TEST_F(PyTablesUnittest, ShrinkToFit) {
  event_.Insert(TestEventTable::Row(100, 0));
  event_.ShrinkToFit();

  // Unfortunately given the loose restrictions on shrink_to_fit provided by the
  // standard library, we cannot really assert anything. Just call the method to
  // ensure it doesn't cause crashes.
}

TEST_F(PyTablesUnittest, FindById) {
  auto id_and_row = event_.Insert(TestEventTable::Row(100, 0));

  auto row_ref = event_.FindById(id_and_row.id);
  ASSERT_EQ(row_ref->ToRowNumber().row_number(), id_and_row.row);
  ASSERT_EQ(row_ref->id(), id_and_row.id);
  ASSERT_EQ(row_ref->ts(), 100);
  ASSERT_EQ(row_ref->arg_set_id(), 0u);
}

TEST_F(PyTablesUnittest, ChildFindById) {
  event_.Insert(TestEventTable::Row(50, 0));
  auto id_and_row = slice_.Insert(TestSliceTable::Row(100, 0, 10));

  auto row_ref = slice_.FindById(id_and_row.id);
  ASSERT_EQ(row_ref->ToRowNumber().row_number(), id_and_row.row);
  ASSERT_EQ(row_ref->id(), id_and_row.id);
  ASSERT_EQ(row_ref->ts(), 100);
  ASSERT_EQ(row_ref->arg_set_id(), 0u);
  ASSERT_EQ(row_ref->dur(), 10u);
}
TEST_F(PyTablesUnittest, ChildTableStatics) {
  ASSERT_EQ(TestSliceTable::ColumnFlag::dur, ColumnLegacy::Flag::kNonNull);
  ASSERT_EQ(TestSliceTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestSliceTable::ColumnIndex::type, 1u);
  ASSERT_EQ(TestSliceTable::ColumnIndex::ts, 2u);
  ASSERT_EQ(TestSliceTable::ColumnIndex::arg_set_id, 3u);
  ASSERT_EQ(TestSliceTable::ColumnIndex::dur, 4u);
}

TEST_F(PyTablesUnittest, ParentAndChildInsert) {
  event_.Insert(TestEventTable::Row(50, 0));
  slice_.Insert(TestSliceTable::Row(100, 1, 10));
  event_.Insert(TestEventTable::Row(150, 2));
  slice_.Insert(TestSliceTable::Row(200, 3, 20));

  ASSERT_EQ(event_.row_count(), 4u);
  ASSERT_EQ(event_[0].id(), TestEventTable::Id{0});
  ASSERT_EQ(pool_.Get(event_[0].type()), "event");
  ASSERT_EQ(event_[0].ts(), 50);

  ASSERT_EQ(event_[1].id(), TestEventTable::Id{1});
  ASSERT_EQ(pool_.Get(event_[1].type()), "slice");
  ASSERT_EQ(event_[1].ts(), 100);

  ASSERT_EQ(event_[2].id(), TestEventTable::Id{2});
  ASSERT_EQ(pool_.Get(event_[2].type()), "event");
  ASSERT_EQ(event_[2].ts(), 150);

  ASSERT_EQ(event_[3].id(), TestEventTable::Id{3});
  ASSERT_EQ(pool_.Get(event_[3].type()), "slice");
  ASSERT_EQ(event_[3].ts(), 200);

  ASSERT_EQ(slice_.row_count(), 2u);
  ASSERT_EQ(slice_[0].id(), TestEventTable::Id{1});
  ASSERT_EQ(pool_.Get(slice_[0].type()), "slice");
  ASSERT_EQ(slice_[0].ts(), 100);
  ASSERT_EQ(slice_[0].dur(), 10);

  ASSERT_EQ(slice_[1].id(), TestEventTable::Id{3});
  ASSERT_EQ(pool_.Get(slice_[1].type()), "slice");
  ASSERT_EQ(slice_[1].ts(), 200);
  ASSERT_EQ(slice_[1].dur(), 20);
}

TEST_F(PyTablesUnittest, Extend) {
  event_.Insert(TestEventTable::Row(50, 0));
  event_.Insert(TestEventTable::Row(100, 1));
  event_.Insert(TestEventTable::Row(150, 2));

  ColumnStorage<int64_t> dur;
  dur.Append(512);
  dur.Append(1024);
  dur.Append(2048);

  auto slice_ext = TestSliceTable::ExtendParent(event_, std::move(dur));
  ASSERT_EQ(slice_ext->row_count(), 3u);
  ASSERT_EQ((*slice_ext)[0].ts(), 50);
  ASSERT_EQ((*slice_ext)[0].dur(), 512);
  ASSERT_EQ((*slice_ext)[1].ts(), 100);
  ASSERT_EQ((*slice_ext)[1].dur(), 1024);
  ASSERT_EQ((*slice_ext)[2].ts(), 150);
  ASSERT_EQ((*slice_ext)[2].dur(), 2048);
}

TEST_F(PyTablesUnittest, SelectAndExtend) {
  event_.Insert(TestEventTable::Row(50, 0));
  event_.Insert(TestEventTable::Row(100, 1));
  event_.Insert(TestEventTable::Row(150, 2));

  std::vector<TestEventTable::RowNumber> rows;
  rows.emplace_back(1);
  ColumnStorage<int64_t> dur;
  dur.Append(1024);

  auto slice_ext = TestSliceTable::SelectAndExtendParent(
      event_, std::move(rows), std::move(dur));
  ASSERT_EQ(slice_ext->row_count(), 1u);
  ASSERT_EQ((*slice_ext)[0].ts(), 100);
  ASSERT_EQ((*slice_ext)[0].dur(), 1024);
}

TEST_F(PyTablesUnittest, SetIdColumns) {
  StringPool pool;
  TestArgsTable table{&pool};

  table.Insert(TestArgsTable::Row(0, 100));
  table.Insert(TestArgsTable::Row(0, 200));
  table.Insert(TestArgsTable::Row(2, 200));
  table.Insert(TestArgsTable::Row(3, 300));
  table.Insert(TestArgsTable::Row(4, 200));
  table.Insert(TestArgsTable::Row(4, 500));
  table.Insert(TestArgsTable::Row(4, 900));
  table.Insert(TestArgsTable::Row(4, 200));
  table.Insert(TestArgsTable::Row(8, 400));

  ASSERT_EQ(table.row_count(), 9u);
  ASSERT_TRUE(table.arg_set_id().IsSetId());

  // Verify that not-present ids are not returned.
  {
    static constexpr uint32_t kFilterArgSetId = 1;
    Query q;
    q.constraints = {table.arg_set_id().eq(kFilterArgSetId)};
    auto res = table.FilterToIterator(q);
    ASSERT_TRUE(!res);
  }
  {
    static constexpr uint32_t kFilterArgSetId = 9;
    Query q;
    q.constraints = {table.arg_set_id().eq(kFilterArgSetId)};
    auto it = table.FilterToIterator(q);
    ASSERT_TRUE(!it);
  }

  // Verify that filtering equality for real arg set ids works as expected.
  {
    static constexpr uint32_t kFilterArgSetId = 4;
    Query q;
    q.constraints = {table.arg_set_id().eq(kFilterArgSetId)};
    uint32_t cnt = 0;
    for (auto it = table.FilterToIterator(q); it; ++it, ++cnt) {
      ASSERT_EQ(it.arg_set_id(), kFilterArgSetId);
    }
    ASSERT_EQ(cnt, 4u);
  }
  {
    static constexpr uint32_t kFilterArgSetId = 0;
    Query q;
    q.constraints = {table.arg_set_id().eq(kFilterArgSetId)};
    uint32_t cnt = 0;
    for (auto it = table.FilterToIterator(q); it; ++it, ++cnt) {
      ASSERT_EQ(it.arg_set_id(), kFilterArgSetId);
    }
    ASSERT_EQ(cnt, 2u);
  }
  {
    static constexpr uint32_t kFilterArgSetId = 8;
    Query q;
    q.constraints = {table.arg_set_id().eq(kFilterArgSetId)};
    uint32_t cnt = 0;
    for (auto it = table.FilterToIterator(q); it; ++it, ++cnt) {
      ASSERT_EQ(it.arg_set_id(), kFilterArgSetId);
    }
    ASSERT_EQ(cnt, 1u);
  }

  // Verify that filtering equality for arg set ids after filtering another
  // column works.
  {
    static constexpr uint32_t kFilterArgSetId = 4;
    Query q;
    q.constraints = {table.int_value().eq(200),
                     table.arg_set_id().eq(kFilterArgSetId)};
    uint32_t cnt = 0;
    for (auto it = table.FilterToIterator(q); it; ++it, ++cnt) {
      ASSERT_EQ(it.arg_set_id(), kFilterArgSetId);
    }
    ASSERT_EQ(cnt, 2u);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::tables
