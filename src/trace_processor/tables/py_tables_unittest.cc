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

#include "src/trace_processor/db/column.h"
#include "src/trace_processor/tables/py_tables_unittest_py.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

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
            Column::Flag::kSorted | Column::Flag::kNonNull);
  ASSERT_EQ(TestEventTable::ColumnFlag::arg_set_id, Column::Flag::kNonNull);
}

TEST_F(PyTablesUnittest, ArgsTableProprties) {
  ASSERT_STREQ(TestArgsTable::Name(), "args");

  ASSERT_EQ(TestArgsTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestArgsTable::ColumnIndex::type, 1u);
  ASSERT_EQ(TestArgsTable::ColumnIndex::arg_set_id, 2u);

  ASSERT_EQ(TestArgsTable::ColumnFlag::arg_set_id, Column::Flag::kSorted |
                                                       Column::Flag::kSetId |
                                                       Column::Flag::kNonNull);
}

TEST_F(PyTablesUnittest, InsertEvent) {
  event_.Insert(TestEventTable::Row(100, 0));

  ASSERT_EQ(event_.type().GetString(0).ToStdString(), "event");
  ASSERT_EQ(event_.ts()[0], 100);
  ASSERT_EQ(event_.arg_set_id()[0], 0u);
}

TEST_F(PyTablesUnittest, InsertEventSpecifyCols) {
  TestEventTable::Row row;
  row.ts = 100;
  row.arg_set_id = 0;
  event_.Insert(row);

  ASSERT_EQ(event_.type().GetString(0).ToStdString(), "event");
  ASSERT_EQ(event_.ts()[0], 100);
  ASSERT_EQ(event_.arg_set_id()[0], 0u);
}

TEST_F(PyTablesUnittest, MutableColumn) {
  event_.Insert(TestEventTable::Row(100, 0));

  ASSERT_EQ((*event_.mutable_ts())[0], 100);
  ASSERT_EQ((*event_.mutable_arg_set_id())[0], 0u);
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
  ASSERT_EQ(TestSliceTable::ColumnFlag::dur, Column::Flag::kNonNull);
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
  ASSERT_EQ(event_.id()[0], TestEventTable::Id{0});
  ASSERT_EQ(event_.type().GetString(0), "event");
  ASSERT_EQ(event_.ts()[0], 50);

  ASSERT_EQ(event_.id()[1], TestEventTable::Id{1});
  ASSERT_EQ(event_.type().GetString(1), "slice");
  ASSERT_EQ(event_.ts()[1], 100);

  ASSERT_EQ(event_.id()[2], TestEventTable::Id{2});
  ASSERT_EQ(event_.type().GetString(2), "event");
  ASSERT_EQ(event_.ts()[2], 150);

  ASSERT_EQ(event_.id()[3], TestEventTable::Id{3});
  ASSERT_EQ(event_.type().GetString(3), "slice");
  ASSERT_EQ(event_.ts()[3], 200);

  ASSERT_EQ(slice_.row_count(), 2u);
  ASSERT_EQ(slice_.id()[0], TestEventTable::Id{1});
  ASSERT_EQ(slice_.type().GetString(0), "slice");
  ASSERT_EQ(slice_.ts()[0], 100);
  ASSERT_EQ(slice_.dur()[0], 10);

  ASSERT_EQ(slice_.id()[1], TestEventTable::Id{3});
  ASSERT_EQ(slice_.type().GetString(1), "slice");
  ASSERT_EQ(slice_.ts()[1], 200);
  ASSERT_EQ(slice_.dur()[1], 20);
}

}  // namespace
}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto
