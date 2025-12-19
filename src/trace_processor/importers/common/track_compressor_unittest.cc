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

#include "src/trace_processor/importers/common/track_compressor.h"

#include <memory>

#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

constexpr auto kNestable = TrackCompressor::SliceBlueprint(
    "nestable",
    tracks::DimensionBlueprints(tracks::kProcessDimensionBlueprint),
    tracks::StaticNameBlueprint("test"));

constexpr auto kUnnestable = TrackCompressor::SliceBlueprint(
    "atrace_async_slice",
    tracks::DimensionBlueprints(tracks::kProcessDimensionBlueprint),
    tracks::StaticNameBlueprint("test"));

class TrackCompressorUnittest : public testing::Test {
 public:
  TrackCompressorUnittest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.track_compressor = std::make_unique<TrackCompressor>(&context_);
    context_.process_track_translation_table =
        std::make_unique<ProcessTrackTranslationTable>(context_.storage.get());
    context_.track_group_idx_state =
        std::make_unique<TrackCompressorGroupIdxState>();

    storage_ = context_.storage.get();
    tracker_ = context_.track_compressor.get();
  }

 protected:
  TraceStorage* storage_ = nullptr;
  TrackCompressor* tracker_ = nullptr;

 private:
  TraceProcessorContext context_;
};

TEST_F(TrackCompressorUnittest, Smoke) {
  auto begin = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 1);
  auto end = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 1);

  ASSERT_EQ(begin, end);

  const auto& process = storage_->track_table();
  auto rr = *process.FindById(begin);
  ASSERT_EQ(rr.upid(), 1u);
  ASSERT_EQ(rr.name(), storage_->string_pool().GetId("test"));
}

TEST_F(TrackCompressorUnittest, EndFirst) {
  auto end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);

  const auto& process = storage_->track_table();
  auto rr = *process.FindById(end);
  ASSERT_EQ(rr.upid(), 1u);
  ASSERT_EQ(rr.name(), storage_->string_pool().GetId("test"));
}

TEST_F(TrackCompressorUnittest, LegacySaturating) {
  auto begin = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 1);
  auto begin_2 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 1);

  ASSERT_EQ(begin, begin_2);
}

TEST_F(TrackCompressorUnittest, DoubleBegin) {
  auto begin = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 1);
  auto end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);
  auto begin_2 = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 1);

  ASSERT_EQ(begin, end);
  ASSERT_EQ(begin, begin_2);
}

TEST_F(TrackCompressorUnittest, Nesting) {
  auto begin = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 1);
  auto begin_nested =
      tracker_->InternBegin(kNestable, tracks::Dimensions(1), 1);
  auto begin_other = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 2);
  auto end_nested = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);
  auto end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);
  auto end_other = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 2);

  ASSERT_EQ(begin, begin_nested);
  ASSERT_NE(begin, begin_other);
  ASSERT_EQ(begin_nested, end_nested);
  ASSERT_EQ(begin, end);
  ASSERT_EQ(begin_other, end_other);
}

TEST_F(TrackCompressorUnittest, NestableMultipleEndAfterBegin) {
  auto begin = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 1);
  auto end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);
  auto end_2 = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);

  ASSERT_EQ(begin, end);
  ASSERT_EQ(end, end_2);
}

TEST_F(TrackCompressorUnittest, OnlyInternScoped) {
  TrackId a = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 100, 10);
  TrackId b = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 105, 2);
  TrackId c = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 107, 3);
  TrackId d = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 110, 5);

  ASSERT_NE(a, b);
  ASSERT_EQ(b, c);
  ASSERT_EQ(a, d);
}

TEST_F(TrackCompressorUnittest, MixInternScopedAndBeginEnd) {
  TrackId a = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 100, 10);

  TrackId begin = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 777);
  TrackId end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 777);

  TrackId b = tracker_->InternScoped(kNestable, tracks::Dimensions(1), 105, 2);

  ASSERT_NE(a, begin);
  ASSERT_NE(b, begin);
  ASSERT_EQ(begin, end);
}

TEST_F(TrackCompressorUnittest, DifferentTracksInterleave) {
  TrackId b1 = tracker_->InternBegin(kNestable, tracks::Dimensions(1), 666);
  TrackId b2 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 777);
  TrackId e1 = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 666);
  TrackId e2 = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b2, e2);
  ASSERT_NE(b1, b2);
}

TEST_F(TrackCompressorUnittest, DifferentCookieInterleave) {
  TrackId b1 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 666);
  TrackId b2 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 777);
  TrackId e1 = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 666);
  TrackId e2 = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b2, e2);
  ASSERT_NE(b1, b2);
}

TEST_F(TrackCompressorUnittest, DifferentCookieSequential) {
  TrackId b1 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 666);
  TrackId e1 = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 666);
  TrackId b2 = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 777);
  TrackId e2 = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 777);

  ASSERT_EQ(b1, e1);
  ASSERT_EQ(b1, b2);
  ASSERT_EQ(b2, e2);
}

}  // namespace
}  // namespace perfetto::trace_processor
