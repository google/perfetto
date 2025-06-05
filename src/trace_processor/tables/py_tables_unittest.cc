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

#include <optional>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/tables/py_tables_unittest_py.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::tables {

namespace {

class PyTablesUnittest : public ::testing::Test {
 protected:
  StringPool pool_;

  TestEventTable event_{&pool_};
  TestArgsTable args_{&pool_};
};

TEST_F(PyTablesUnittest, EventTableProperties) {
  ASSERT_STREQ(TestEventTable::Name(), "event");

  ASSERT_EQ(TestEventTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestEventTable::ColumnIndex::ts, 1u);
  ASSERT_EQ(TestEventTable::ColumnIndex::arg_set_id, 2u);
}

TEST_F(PyTablesUnittest, ArgsTableProperties) {
  ASSERT_STREQ(TestArgsTable::Name(), "args");

  ASSERT_EQ(TestArgsTable::ColumnIndex::id, 0u);
  ASSERT_EQ(TestArgsTable::ColumnIndex::arg_set_id, 1u);
}

TEST_F(PyTablesUnittest, InsertEvent) {
  event_.Insert(TestEventTable::Row(100, 0));

  ASSERT_EQ(event_[0].ts(), 100);
  ASSERT_EQ(event_[0].arg_set_id(), 0u);
}

TEST_F(PyTablesUnittest, InsertEventSpecifyCols) {
  TestEventTable::Row row;
  row.ts = 100;
  row.arg_set_id = std::nullopt;
  event_.Insert(row);

  ASSERT_EQ(event_[0].ts(), 100);
  ASSERT_EQ(event_[0].arg_set_id(), std::nullopt);
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

}  // namespace
}  // namespace perfetto::trace_processor::tables
