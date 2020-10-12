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

#include "src/trace_processor/importers/proto/async_track_set_tracker.h"

#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class AsyncTrackSetTrackerUnittest {
 public:
  AsyncTrackSetTrackerUnittest() {
    context_.storage.reset(new TraceStorage());
    context_.track_tracker.reset(new TrackTracker(&context_));
    context_.async_track_set_tracker.reset(new AsyncTrackSetTracker(&context_));

    storage_ = context_.storage.get();
    tracker_ = context_.async_track_set_tracker.get();
  }

 protected:
  TraceStorage* storage_ = nullptr;
  AsyncTrackSetTracker* tracker_ = nullptr;

 private:
  TraceProcessorContext context_;
};

TEST_F(AsyncTrackSetTrackerUnittest, Smoke) {
  auto set_id = tracker_->InternAndroidSet(1, storage_->InternString("test"));

  auto begin = tracker_->Begin(
      set_id, 1,
      AsyncTrackSetTracker::NestingType::kLegacySaturatingUnnestable);
  auto end = tracker_->End(set_id, 1);

  ASSERT_EQ(begin, end);

  uint32_t row = *storage_->process_track_table().id().IndexOf(begin);
  ASSERT_EQ(storage_->process_track_table().upid()[row], 1);
  ASSERT_EQ(storage_->process_track_table().name()[row],
            storage_->InternString("test"));
}

TEST_F(AsyncTrackSetTrackerUnittest, EndFirst) {
  auto set_id = tracker_->InternAndroidSet(1, storage_->InternString("test"));
  auto end = tracker_->End(set_id, 1);

  uint32_t row = *storage_->process_track_table().id().IndexOf(end);
  ASSERT_EQ(storage_->process_track_table().upid()[row], 1);
  ASSERT_EQ(storage_->process_track_table().name()[row],
            storage_->InternString("test"));
}

TEST_F(AsyncTrackSetTrackerUnittest, LegacySaturating) {
  auto set_id = tracker_->InternAndroidSet(1, storage_->InternString("test"));

  auto begin = tracker_->Begin(
      set_id, 1,
      AsyncTrackSetTracker::NestingType::kLegacySaturatingUnnestable);
  auto begin_2 = tracker_->Begin(
      set_id, 1,
      AsyncTrackSetTracker::NestingType::kLegacySaturatingUnnestable);

  ASSERT_EQ(begin, begin_2);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
