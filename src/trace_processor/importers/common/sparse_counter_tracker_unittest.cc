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

#include "src/trace_processor/importers/common/sparse_counter_tracker.h"

#include <memory>

#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class SparseCounterTrackerTest : public ::testing::Test {
 public:
  SparseCounterTrackerTest() {
    context.storage = std::make_unique<TraceStorage>();
    context.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context.storage.get());
    context.event_tracker = std::make_unique<EventTracker>(&context);
    context.track_tracker = std::make_unique<TrackTracker>(&context);
    context.sorter = std::make_unique<TraceSorter>(
        &context, TraceSorter::SortingMode::kFullSort);
    tracker = std::make_unique<SparseCounterTracker>(&context);
  }

 protected:
  TraceProcessorContext context;
  std::unique_ptr<SparseCounterTracker> tracker;
};

TEST_F(SparseCounterTrackerTest, Basic) {
  TrackId track{1};

  tracker->PushCounter(10, track, 5);
  tracker->PushCounter(20, track, 10);
  tracker->PushCounter(30, track, 10);
  tracker->PushCounter(40, track, 10);
  tracker->PushCounter(50, track, 15);

  context.sorter->ExtractEventsForced();

  const auto& counter = context.storage->counter_table();
  ASSERT_EQ(counter.row_count(), 4ul);

  EXPECT_EQ(counter[0].ts(), 10);
  EXPECT_DOUBLE_EQ(counter[0].value(), 5);

  EXPECT_EQ(counter[1].ts(), 20);
  EXPECT_DOUBLE_EQ(counter[1].value(), 10);

  EXPECT_EQ(counter[2].ts(), 40);
  EXPECT_DOUBLE_EQ(counter[2].value(), 10);

  EXPECT_EQ(counter[3].ts(), 50);
  EXPECT_DOUBLE_EQ(counter[3].value(), 15);
}

TEST_F(SparseCounterTrackerTest, MultipleUpdates) {
  TrackId track{1};

  tracker->PushCounter(10, track, 5);
  tracker->PushCounter(20, track, 5);
  tracker->PushCounter(30, track, 5);
  tracker->PushCounter(40, track, 5);
  tracker->PushCounter(40, track, 10);

  context.sorter->ExtractEventsForced();

  const auto& counter = context.storage->counter_table();
  ASSERT_EQ(counter.row_count(), 3ul);

  EXPECT_EQ(counter[0].ts(), 10);
  EXPECT_DOUBLE_EQ(counter[0].value(), 5);

  EXPECT_EQ(counter[1].ts(), 40);
  EXPECT_DOUBLE_EQ(counter[1].value(), 5);

  EXPECT_EQ(counter[2].ts(), 40);
  EXPECT_DOUBLE_EQ(counter[2].value(), 10);
}

}  // namespace
}  // namespace perfetto::trace_processor
