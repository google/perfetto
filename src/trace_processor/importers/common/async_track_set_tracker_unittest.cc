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

#include "src/trace_processor/importers/common/async_track_set_tracker.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

class AsyncTrackSetTrackerUnittest : public testing::Test {
 public:
  AsyncTrackSetTrackerUnittest() {
    context_.storage.reset(new TraceStorage());
    context_.global_args_tracker.reset(
        new GlobalArgsTracker(context_.storage.get()));
    context_.args_tracker.reset(new ArgsTracker(&context_));
    context_.track_tracker.reset(new TrackTracker(&context_));
    context_.async_track_set_tracker.reset(new AsyncTrackSetTracker(&context_));

    storage_ = context_.storage.get();
    tracker_ = context_.async_track_set_tracker.get();

    nestable_id_ =
        tracker_->InternProcessTrackSet(1, storage_->InternString("test"));
    legacy_unnestable_id_ = tracker_->InternAndroidLegacyUnnestableTrackSet(
        2, storage_->InternString("test"));
  }

 protected:
  TraceStorage* storage_ = nullptr;
  AsyncTrackSetTracker* tracker_ = nullptr;

  AsyncTrackSetTracker::TrackSetId nestable_id_;
  AsyncTrackSetTracker::TrackSetId legacy_unnestable_id_;

 private:
  TraceProcessorContext context_;
};

namespace {

TEST_F(AsyncTrackSetTrackerUnittest, Smoke) {
  auto set_id = tracker_->InternAndroidLegacyUnnestableTrackSet(
      1, storage_->InternString("test"));

  auto begin = tracker_->Begin(set_id, 1);
  auto end = tracker_->End(set_id, 1);

  ASSERT_EQ(begin, end);

  uint32_t row = *storage_->process_track_table().id().IndexOf(begin);
  ASSERT_EQ(storage_->process_track_table().upid()[row], 1u);
  ASSERT_EQ(storage_->process_track_table().name()[row],
            storage_->InternString("test"));
}

TEST_F(AsyncTrackSetTrackerUnittest, EndFirst) {
  auto end = tracker_->End(nestable_id_, 1);

  uint32_t row = *storage_->process_track_table().id().IndexOf(end);
  ASSERT_EQ(storage_->process_track_table().upid()[row], 1u);
  ASSERT_EQ(storage_->process_track_table().name()[row],
            storage_->InternString("test"));
}

TEST_F(AsyncTrackSetTrackerUnittest, LegacySaturating) {
  auto begin = tracker_->Begin(legacy_unnestable_id_, 1);
  auto begin_2 = tracker_->Begin(legacy_unnestable_id_, 1);

  ASSERT_EQ(begin, begin_2);
}

TEST_F(AsyncTrackSetTrackerUnittest, DoubleBegin) {
  auto begin = tracker_->Begin(nestable_id_, 1);
  auto end = tracker_->End(nestable_id_, 1);
  auto begin_2 = tracker_->Begin(nestable_id_, 1);

  ASSERT_EQ(begin, end);
  ASSERT_EQ(begin, begin_2);
}

TEST_F(AsyncTrackSetTrackerUnittest, Nesting) {
  auto begin = tracker_->Begin(nestable_id_, 1);
  auto begin_nested = tracker_->Begin(nestable_id_, 1);
  auto begin_other = tracker_->Begin(nestable_id_, 2);
  auto end_nested = tracker_->End(nestable_id_, 1);
  auto end = tracker_->End(nestable_id_, 1);
  auto end_other = tracker_->Begin(nestable_id_, 2);

  ASSERT_EQ(begin, begin_nested);
  ASSERT_NE(begin, begin_other);
  ASSERT_EQ(begin_nested, end_nested);
  ASSERT_EQ(begin, end);
  ASSERT_EQ(begin_other, end_other);
}

TEST_F(AsyncTrackSetTrackerUnittest, NestableMultipleEndAfterBegin) {
  auto begin = tracker_->Begin(nestable_id_, 1);
  auto end = tracker_->End(nestable_id_, 1);
  auto end_2 = tracker_->End(nestable_id_, 1);

  ASSERT_EQ(begin, end);
  ASSERT_EQ(end, end_2);
}

TEST_F(AsyncTrackSetTrackerUnittest, OnlyScoped) {
  TrackId a = tracker_->Scoped(nestable_id_, 100, 10);
  TrackId b = tracker_->Scoped(nestable_id_, 105, 2);
  TrackId c = tracker_->Scoped(nestable_id_, 107, 3);
  TrackId d = tracker_->Scoped(nestable_id_, 110, 5);

  ASSERT_NE(a, b);
  ASSERT_EQ(b, c);
  ASSERT_EQ(a, d);
}

TEST_F(AsyncTrackSetTrackerUnittest, MixScopedAndBeginEnd) {
  TrackId a = tracker_->Scoped(nestable_id_, 100, 10);

  TrackId begin = tracker_->Begin(nestable_id_, 777);
  TrackId end = tracker_->End(nestable_id_, 777);

  TrackId b = tracker_->Scoped(nestable_id_, 105, 2);

  ASSERT_NE(a, begin);
  ASSERT_NE(b, begin);
  ASSERT_EQ(begin, end);
}

TEST_F(AsyncTrackSetTrackerUnittest, DifferentTracksInterleave) {
  TrackId b1 = tracker_->Begin(nestable_id_, 666);
  TrackId b2 = tracker_->Begin(legacy_unnestable_id_, 777);
  TrackId e1 = tracker_->End(nestable_id_, 666);
  TrackId e2 = tracker_->End(legacy_unnestable_id_, 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b2, e2);
  ASSERT_NE(b1, b2);
}

TEST_F(AsyncTrackSetTrackerUnittest, DifferentCookieInterleave) {
  TrackId b1 = tracker_->Begin(legacy_unnestable_id_, 666);
  TrackId b2 = tracker_->Begin(legacy_unnestable_id_, 777);
  TrackId e1 = tracker_->End(legacy_unnestable_id_, 666);
  TrackId e2 = tracker_->End(legacy_unnestable_id_, 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b2, e2);
  ASSERT_NE(b1, b2);
}

TEST_F(AsyncTrackSetTrackerUnittest, DifferentCookieSequential) {
  TrackId b1 = tracker_->Begin(legacy_unnestable_id_, 666);
  TrackId e1 = tracker_->End(legacy_unnestable_id_, 666);
  TrackId b2 = tracker_->Begin(legacy_unnestable_id_, 777);
  TrackId e2 = tracker_->End(legacy_unnestable_id_, 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b1, b2);
  ASSERT_EQ(b2, e2);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
