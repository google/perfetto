/*
 * Copyright (C) 2021 The Android Open Source Project
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
#include "src/trace_processor/dynamic/experimental_flat_slice_generator.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

tables::SliceTable::Row SliceRow(int64_t ts,
                                 int64_t dur,
                                 uint32_t depth,
                                 TrackId track_id) {
  tables::SliceTable::Row row;
  row.ts = ts;
  row.dur = dur;
  row.depth = depth;
  row.track_id = track_id;
  return row;
}

class TableAsserter {
 public:
  TableAsserter(Table table) : table_(std::move(table)) {}

  void NextSlice(int64_t ts, int64_t dur) {
    ++idx_;
    ASSERT_EQ(table_.GetTypedColumnByName<int64_t>("ts")[idx_], ts)
        << "where idx_ = " << idx_;
    ASSERT_EQ(table_.GetTypedColumnByName<int64_t>("dur")[idx_], dur)
        << "where idx_ = " << idx_;
  }

 private:
  Table table_;
  uint32_t idx_ = std::numeric_limits<uint32_t>::max();
};

TEST(ExperimentalFlatSliceGenerator, Smoke) {
  StringPool pool;
  tables::SliceTable table(&pool, nullptr);

  // A simple stack on track 1.
  table.Insert(SliceRow(100, 10, 0, TrackId{1}));
  table.Insert(SliceRow(104, 6, 1, TrackId{1}));
  table.Insert(SliceRow(107, 1, 2, TrackId{1}));

  // Back to back slices with a gap on track 2.
  table.Insert(SliceRow(200, 10, 0, TrackId{2}));
  table.Insert(SliceRow(210, 10, 0, TrackId{2}));
  table.Insert(SliceRow(230, 10, 0, TrackId{2}));

  // Deep nesting on track 3.
  table.Insert(SliceRow(300, 100, 0, TrackId{3}));
  table.Insert(SliceRow(301, 98, 1, TrackId{3}));
  table.Insert(SliceRow(302, 96, 2, TrackId{3}));
  table.Insert(SliceRow(303, 94, 3, TrackId{3}));
  table.Insert(SliceRow(304, 92, 4, TrackId{3}));
  table.Insert(SliceRow(305, 90, 5, TrackId{3}));

  auto out = ExperimentalFlatSliceGenerator::ComputeFlatSliceTable(table, &pool,
                                                                   0, 400);
  auto sorted = out->Sort({out->track_id().ascending(), out->ts().ascending()});

  ASSERT_EQ(sorted.row_count(), 27u);
  TableAsserter asserter(std::move(sorted));

  // Track 1's slices.
  asserter.NextSlice(0, 100);
  asserter.NextSlice(100, 4);
  asserter.NextSlice(104, 3);
  asserter.NextSlice(107, 1);
  asserter.NextSlice(108, 2);
  asserter.NextSlice(110, 0);
  asserter.NextSlice(110, 290);

  // Track 2's slices.
  asserter.NextSlice(0, 200);
  asserter.NextSlice(200, 10);
  asserter.NextSlice(210, 0);
  asserter.NextSlice(210, 10);
  asserter.NextSlice(220, 10);
  asserter.NextSlice(230, 10);
  asserter.NextSlice(240, 160);

  // Track 3's slices.
  asserter.NextSlice(0, 300);
  asserter.NextSlice(300, 1);
  asserter.NextSlice(301, 1);
  asserter.NextSlice(302, 1);
  asserter.NextSlice(303, 1);
  asserter.NextSlice(304, 1);
  asserter.NextSlice(305, 90);
  asserter.NextSlice(395, 1);
  asserter.NextSlice(396, 1);
  asserter.NextSlice(397, 1);
  asserter.NextSlice(398, 1);
  asserter.NextSlice(399, 1);
  asserter.NextSlice(400, 0);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
