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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_counter_dur.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

tables::CounterTable::Row CounterRow(int64_t ts, uint32_t track_id) {
  tables::CounterTable::Row row;
  row.ts = ts;
  row.track_id = tables::TrackTable::Id{track_id};
  return row;
}

TEST(ExperimentalCounterDur, SmokeDur) {
  StringPool pool;
  tables::CounterTable table(&pool);

  table.Insert(CounterRow(100 /* ts */, 1 /* track_id */));
  table.Insert(CounterRow(102 /* ts */, 2 /* track_id */));
  table.Insert(CounterRow(105 /* ts */, 1 /* track_id */));
  table.Insert(CounterRow(105 /* ts */, 3 /* track_id */));
  table.Insert(CounterRow(105 /* ts */, 2 /* track_id */));
  table.Insert(CounterRow(110 /* ts */, 2 /* track_id */));

  auto dur = ExperimentalCounterDur::ComputeDurColumn(table);
  ASSERT_EQ(dur.size(), table.row_count());

  ASSERT_EQ(dur.Get(0), 5);
  ASSERT_EQ(dur.Get(1), 3);
  ASSERT_EQ(dur.Get(2), -1);
  ASSERT_EQ(dur.Get(3), -1);
  ASSERT_EQ(dur.Get(4), 5);
  ASSERT_EQ(dur.Get(5), -1);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
