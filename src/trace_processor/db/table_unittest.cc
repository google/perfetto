/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/db/table.h"
#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/db/typed_column.h"
#include "src/trace_processor/tables/macros.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

#define PERFETTO_TP_TEST_EVENT_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestEventTable, "event")                           \
  PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)            \
  C(int64_t, ts, Column::Flag::kSorted)                   \
  C(int64_t, dur)                                         \
  C(uint32_t, arg_set_id, Column::Flag::kSorted | Column::Flag::kSetId)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_EVENT_TABLE_DEF);

TestEventTable::~TestEventTable() = default;

TEST(TableTest, SetIdColumns) {
  StringPool pool;
  TestEventTable table{&pool, nullptr};

  table.Insert(TestEventTable::Row(0, 0, 0));
  table.Insert(TestEventTable::Row(1, 0, 0));
  table.Insert(TestEventTable::Row(2, 0, 2));
  table.Insert(TestEventTable::Row(3, 0, 3));
  table.Insert(TestEventTable::Row(4, 0, 4));
  table.Insert(TestEventTable::Row(5, 0, 4));
  table.Insert(TestEventTable::Row(6, 0, 4));
  table.Insert(TestEventTable::Row(7, 0, 4));
  table.Insert(TestEventTable::Row(8, 0, 8));

  ASSERT_EQ(table.row_count(), 9u);
  ASSERT_TRUE(table.arg_set_id().IsSetId());

  // Verify that not-present ids are not returned.
  {
    static constexpr uint32_t kFilterArgSetId = 1;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 0u);
  }
  {
    static constexpr uint32_t kFilterArgSetId = 9;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 0u);
  }

  // Verify that kSetId flag is correctly removed after filtering/sorting.
  {
    static constexpr uint32_t kFilterArgSetId = 3;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 1u);
    ASSERT_FALSE(res.GetColumnByName("arg_set_id")->IsSetId());
  }
  {
    auto res = table.Sort({table.dur().descending()});
    ASSERT_FALSE(res.GetColumnByName("arg_set_id")->IsSetId());
  }

  uint32_t arg_set_id_col_idx =
      static_cast<uint32_t>(TestEventTable::ColumnIndex::arg_set_id);

  // Verify that filtering equality for real arg set ids works as expected.
  {
    static constexpr uint32_t kFilterArgSetId = 4;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 4u);
    for (auto it = res.IterateRows(); it; it.Next()) {
      uint32_t arg_set_id =
          static_cast<uint32_t>(it.Get(arg_set_id_col_idx).AsLong());
      ASSERT_EQ(arg_set_id, kFilterArgSetId);
    }
  }
  {
    static constexpr uint32_t kFilterArgSetId = 0;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 2u);
    for (auto it = res.IterateRows(); it; it.Next()) {
      uint32_t arg_set_id =
          static_cast<uint32_t>(it.Get(arg_set_id_col_idx).AsLong());
      ASSERT_EQ(arg_set_id, kFilterArgSetId);
    }
  }
  {
    static constexpr uint32_t kFilterArgSetId = 8;
    auto res = table.Filter({table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 1u);
    for (auto it = res.IterateRows(); it; it.Next()) {
      uint32_t arg_set_id =
          static_cast<uint32_t>(it.Get(arg_set_id_col_idx).AsLong());
      ASSERT_EQ(arg_set_id, kFilterArgSetId);
    }
  }

  // Verify that filtering equality for arg set ids after filtering another
  // column works.
  {
    static constexpr uint32_t kFilterArgSetId = 4;
    auto res = table.Filter(
        {table.ts().ge(6), table.arg_set_id().eq(kFilterArgSetId)});
    ASSERT_EQ(res.row_count(), 2u);
    for (auto it = res.IterateRows(); it; it.Next()) {
      uint32_t arg_set_id =
          static_cast<uint32_t>(it.Get(arg_set_id_col_idx).AsLong());
      ASSERT_EQ(arg_set_id, kFilterArgSetId);
    }
  }
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
