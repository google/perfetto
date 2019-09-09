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

#include "src/trace_processor/tables/macros.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

#define PERFETTO_TP_TEST_EVENT_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestEventTable, "event")                           \
  PARENT(PERFETTO_TP_ROOT_TABLE_PARENT_DEF, C)            \
  C(int64_t, ts)                                          \
  C(int64_t, arg_set_id)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_EVENT_TABLE_DEF);

#define PERFETTO_TP_TEST_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestSliceTable, "slice")                           \
  PARENT(PERFETTO_TP_TEST_EVENT_TABLE_DEF, C)             \
  C(base::Optional<int64_t>, dur)                         \
  C(int64_t, depth)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_SLICE_TABLE_DEF);

#define PERFETTO_TP_TEST_CPU_SLICE_TABLE_DEF(NAME, PARENT, C) \
  NAME(TestCpuSliceTable, "cpu_slice")                        \
  PARENT(PERFETTO_TP_TEST_SLICE_TABLE_DEF, C)                 \
  C(int64_t, cpu)                                             \
  C(int64_t, priority)                                        \
  C(StringPool::Id, end_state)
PERFETTO_TP_TABLE(PERFETTO_TP_TEST_CPU_SLICE_TABLE_DEF);

TEST(TableMacrosUnittest, Name) {
  StringPool pool;
  TestEventTable event(&pool, nullptr);
  TestSliceTable slice(&pool, &event);
  TestCpuSliceTable cpu_slice(&pool, &slice);

  ASSERT_EQ(event.table_name(), "event");
  ASSERT_EQ(slice.table_name(), "slice");
  ASSERT_EQ(cpu_slice.table_name(), "cpu_slice");
}

TEST(TableMacrosUnittest, InsertParent) {
  StringPool pool;
  TestEventTable event(&pool, nullptr);
  TestSliceTable slice(&pool, &event);

  uint32_t id = event.Insert(TestEventTable::Row(100, 0));
  ASSERT_EQ(id, 0u);
  ASSERT_EQ(event.type().GetString(0), "event");
  ASSERT_EQ(event.ts()[0], 100);
  ASSERT_EQ(event.arg_set_id()[0], 0);

  id = slice.Insert(TestSliceTable::Row(200, 123, 10, 0));
  ASSERT_EQ(id, 1u);

  ASSERT_EQ(event.type().GetString(1), "slice");
  ASSERT_EQ(event.ts()[1], 200);
  ASSERT_EQ(event.arg_set_id()[1], 123);
  ASSERT_EQ(slice.type().GetString(0), "slice");
  ASSERT_EQ(slice.ts()[0], 200);
  ASSERT_EQ(slice.arg_set_id()[0], 123);
  ASSERT_EQ(slice.dur()[0], 10);
  ASSERT_EQ(slice.depth()[0], 0);

  id = slice.Insert(TestSliceTable::Row(210, 456, base::nullopt, 0));
  ASSERT_EQ(id, 2u);

  ASSERT_EQ(event.type().GetString(2), "slice");
  ASSERT_EQ(event.ts()[2], 210);
  ASSERT_EQ(event.arg_set_id()[2], 456);
  ASSERT_EQ(slice.type().GetString(1), "slice");
  ASSERT_EQ(slice.ts()[1], 210);
  ASSERT_EQ(slice.arg_set_id()[1], 456);
  ASSERT_EQ(slice.dur()[1], base::nullopt);
  ASSERT_EQ(slice.depth()[1], 0);
}

TEST(TableMacrosUnittest, InsertChild) {
  StringPool pool;
  TestEventTable event(&pool, nullptr);
  TestSliceTable slice(&pool, &event);
  TestCpuSliceTable cpu_slice(&pool, &slice);

  event.Insert(TestEventTable::Row(100, 0));
  slice.Insert(TestSliceTable::Row(200, 123, 10, 0));

  auto reason = pool.InternString("R");
  uint32_t id =
      cpu_slice.Insert(TestCpuSliceTable::Row(205, 456, 5, 1, 4, 1024, reason));
  ASSERT_EQ(id, 2u);
  ASSERT_EQ(event.type().GetString(2), "cpu_slice");
  ASSERT_EQ(event.ts()[2], 205);
  ASSERT_EQ(event.arg_set_id()[2], 456);

  ASSERT_EQ(slice.type().GetString(1), "cpu_slice");
  ASSERT_EQ(slice.ts()[1], 205);
  ASSERT_EQ(slice.arg_set_id()[1], 456);
  ASSERT_EQ(slice.dur()[1], 5);
  ASSERT_EQ(slice.depth()[1], 1);

  ASSERT_EQ(cpu_slice.type().GetString(0), "cpu_slice");
  ASSERT_EQ(cpu_slice.ts()[0], 205);
  ASSERT_EQ(cpu_slice.arg_set_id()[0], 456);
  ASSERT_EQ(cpu_slice.dur()[0], 5);
  ASSERT_EQ(cpu_slice.depth()[0], 1);
  ASSERT_EQ(cpu_slice.cpu()[0], 4);
  ASSERT_EQ(cpu_slice.priority()[0], 1024);
  ASSERT_EQ(cpu_slice.end_state()[0], reason);
  ASSERT_EQ(cpu_slice.end_state().GetString(0), "R");
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
