/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/views/macros.h"

#include "src/trace_processor/views/macros_unittest_py.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace tables {

#define PERFETTO_TP_TEST_EVENT_TABLE_DEF(NAME, PARENT, C) \
  NAME(MacrosEventTable, "event")                         \
  C(int64_t, ts, Column::Flag::kSorted)                   \
  C(MacrosThreadTable::Id, thread_id)

MacrosEventTable::~MacrosEventTable() = default;
MacrosThreadTable::~MacrosThreadTable() = default;

namespace {

#define PERFETTO_TP_EVENT_VIEW_DEF(NAME, FROM, JOIN, COL, FCOL)             \
  NAME(TestEventView, "event_view")                                         \
  PERFETTO_TP_VIEW_EXPORT_FROM_COLS(PERFETTO_TP_TEST_EVENT_TABLE_DEF, FCOL) \
  COL(thread_name, thread, name)                                            \
  COL(thread_start_ts, thread, start_ts)                                    \
  FROM(MacrosEventTable, event)                                             \
  JOIN(MacrosThreadTable, thread, id, event, thread_id, View::kIdAlwaysPresent)
PERFETTO_TP_DECLARE_VIEW(PERFETTO_TP_EVENT_VIEW_DEF);
PERFETTO_TP_DEFINE_VIEW(TestEventView);

TEST(ViewMacrosUnittest, ColIdx) {
  // Note: inlining these will cause myserious linker errors which don't have a
  // good explanation as to their cause.
  static constexpr uint32_t id = TestEventView::ColumnIndex::id;
  static constexpr uint32_t type = TestEventView::ColumnIndex::type;
  static constexpr uint32_t ts = TestEventView::ColumnIndex::ts;
  static constexpr uint32_t thread_id = TestEventView::ColumnIndex::thread_id;
  static constexpr uint32_t thread_name =
      TestEventView::ColumnIndex::thread_name;
  static constexpr uint32_t thread_start_ts =
      TestEventView::ColumnIndex::thread_start_ts;

  ASSERT_EQ(id, 0u);
  ASSERT_EQ(type, 1u);
  ASSERT_EQ(ts, 2u);
  ASSERT_EQ(thread_id, 3u);
  ASSERT_EQ(thread_name, 4u);
  ASSERT_EQ(thread_start_ts, 5u);
}

TEST(ViewMacrosUnittest, Schema) {
  MacrosThreadTable thread{nullptr};
  MacrosEventTable event{nullptr};

  TestEventView view{&event, &thread};
  auto schema = view.schema();

  ASSERT_EQ(schema.columns.size(), 6u);

  ASSERT_EQ(schema.columns[0].name, "id");
  ASSERT_EQ(schema.columns[0].is_id, true);
  ASSERT_EQ(schema.columns[0].is_sorted, true);

  ASSERT_EQ(schema.columns[1].name, "type");

  ASSERT_EQ(schema.columns[2].name, "ts");
  ASSERT_TRUE(schema.columns[2].is_sorted);

  ASSERT_EQ(schema.columns[3].name, "thread_id");

  ASSERT_EQ(schema.columns[4].name, "thread_name");

  ASSERT_EQ(schema.columns[5].name, "thread_start_ts");
  ASSERT_FALSE(schema.columns[5].is_sorted);
}

}  // namespace
}  // namespace tables
}  // namespace trace_processor
}  // namespace perfetto
