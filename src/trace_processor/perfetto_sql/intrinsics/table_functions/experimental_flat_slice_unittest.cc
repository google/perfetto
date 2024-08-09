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
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flat_slice.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class TableInseter {
 public:
  void Insert(int64_t ts, int64_t dur, uint32_t depth, TrackId track_id) {
    tables::SliceTable::Row row;
    row.ts = ts;
    row.dur = dur;
    row.depth = depth;
    row.track_id = track_id;
    rows_.emplace_back(row);
  }

  void Populate(tables::SliceTable& table) {
    using R = tables::SliceTable::Row;
    std::sort(rows_.begin(), rows_.end(),
              [](const R& a, const R& b) { return a.ts < b.ts; });
    for (const auto& row : rows_) {
      table.Insert(row);
    }
    rows_.clear();
  }

 private:
  std::vector<tables::SliceTable::Row> rows_;
};

class TableAsserter {
 public:
  explicit TableAsserter(tables::ExperimentalFlatSliceTable::Iterator it)
      : iterator_(std::move(it)) {}

  void NextSlice(int64_t ts, int64_t dur) {
    ASSERT_TRUE(HasMoreSlices());
    ASSERT_EQ(iterator_.ts(), ts);
    ASSERT_EQ(iterator_.dur(), dur);
    ++iterator_;
  }

  bool HasMoreSlices() { return bool(iterator_); }

 private:
  tables::ExperimentalFlatSliceTable::Iterator iterator_;
};

TEST(ExperimentalFlatSlice, Smoke) {
  StringPool pool;
  TableInseter inserter;
  tables::SliceTable table(&pool);

  // A simple stack on track 1.
  inserter.Insert(100, 10, 0, TrackId{1});
  inserter.Insert(104, 6, 1, TrackId{1});
  inserter.Insert(107, 1, 2, TrackId{1});

  // Back to back slices with a gap on track 2.
  inserter.Insert(200, 10, 0, TrackId{2});
  inserter.Insert(210, 10, 0, TrackId{2});
  inserter.Insert(230, 10, 0, TrackId{2});

  // Deep nesting on track 3.
  inserter.Insert(300, 100, 0, TrackId{3});
  inserter.Insert(301, 98, 1, TrackId{3});
  inserter.Insert(302, 96, 2, TrackId{3});
  inserter.Insert(303, 94, 3, TrackId{3});
  inserter.Insert(304, 92, 4, TrackId{3});
  inserter.Insert(305, 90, 5, TrackId{3});

  // Populate the table.
  inserter.Populate(table);

  auto out = ExperimentalFlatSlice::ComputeFlatSliceTable(table, &pool, 0, 400);
  Query q;
  q.orders = {out->track_id().ascending(), out->ts().ascending()};
  auto it = out->FilterToIterator(q);

  TableAsserter asserter(std::move(it));

  // Track 1's slices.
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(0, 100));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(100, 4));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(104, 3));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(107, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(108, 2));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(110, 0));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(110, 290));

  // Track 2's slices.
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(0, 200));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(200, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(210, 0));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(210, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(220, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(230, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(240, 160));

  // Track 3's slices.
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(0, 300));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(300, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(301, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(302, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(303, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(304, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(305, 90));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(395, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(396, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(397, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(398, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(399, 1));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(400, 0));

  ASSERT_FALSE(asserter.HasMoreSlices());
}

TEST(ExperimentalFlatSlice, Bounds) {
  StringPool pool;
  TableInseter inserter;
  tables::SliceTable table(&pool);

  /// Our timebounds is between 200 and 300.
  int64_t start = 200;
  int64_t end = 300;

  // Track 1 has all events inside bounds.
  inserter.Insert(200, 10, 0, TrackId{1});
  inserter.Insert(210, 10, 0, TrackId{1});
  inserter.Insert(230, 10, 0, TrackId{1});

  // Track 2 has a two stacks, first partially inside at start, second partially
  // inside at end.
  // First stack.
  inserter.Insert(190, 20, 0, TrackId{2});
  inserter.Insert(200, 9, 1, TrackId{2});
  inserter.Insert(201, 1, 2, TrackId{2});

  // Second stack.
  inserter.Insert(290, 20, 0, TrackId{2});
  inserter.Insert(299, 2, 1, TrackId{2});
  inserter.Insert(300, 1, 2, TrackId{2});

  // Track 3 has two stacks but *only* outside bounds.
  inserter.Insert(190, 9, 0, TrackId{3});
  inserter.Insert(195, 2, 1, TrackId{3});

  inserter.Insert(300, 9, 0, TrackId{3});
  inserter.Insert(301, 2, 1, TrackId{3});

  // Track 4 has one stack which is partially inside at start.
  inserter.Insert(190, 20, 0, TrackId{4});
  inserter.Insert(201, 2, 1, TrackId{4});

  // Populate the table.
  inserter.Populate(table);

  auto out =
      ExperimentalFlatSlice::ComputeFlatSliceTable(table, &pool, start, end);
  Query q;
  q.orders = {out->track_id().ascending(), out->ts().ascending()};
  auto it = out->FilterToIterator(q);

  TableAsserter asserter(std::move(it));

  // Track 1's slices.
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(200, 0));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(200, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(210, 0));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(210, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(220, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(230, 10));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(240, 60));

  // Track 2's slices.
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(200, 90));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(290, 9));
  ASSERT_NO_FATAL_FAILURE(asserter.NextSlice(299, 1));

  ASSERT_FALSE(asserter.HasMoreSlices());
}

}  // namespace
}  // namespace perfetto::trace_processor
