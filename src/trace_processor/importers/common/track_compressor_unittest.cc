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

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/args_utils.h"
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
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.args_tracker =
        std::make_unique<ArgsTracker>(context_.storage.get());
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.track_compressor = std::make_unique<TrackCompressor>(&context_);
    context_.process_track_translation_table =
        std::make_unique<ProcessTrackTranslationTable>(context_.storage.get());
    context_.track_group_idx_state =
        std::make_unique<TrackCompressorGroupIdxState>();

    storage_ = context_.storage.get();
    tracker_ = context_.track_compressor.get();
    track_tracker_ = context_.track_tracker.get();
  }

 protected:
  TraceStorage* storage_ = nullptr;
  TrackCompressor* tracker_ = nullptr;
  TrackTracker* track_tracker_ = nullptr;

 private:
  TraceProcessorContext context_;
};

TEST_F(TrackCompressorUnittest, AdditionalDimensionsInEveryLane) {
  auto factory = [&] {
    std::vector<ArgsTracker::CompactArg> dimensions;
    for (uint32_t i = 0; i < 25; ++i) {
      StringId key =
          storage_->InternString("custom_dimension.d" + std::to_string(i));
      dimensions.emplace_back(
          ArgsTracker::CompactArg{key, key, Variadic::Integer(i)});
    }
    return tracker_->CreateTrackFactoryWithDimensions(
        kNestable, tracks::Dimensions(1),
        TrackTracker::MakeAdditionalDimensions(std::move(dimensions)));
  }();
  TrackId first = tracker_->Begin(factory, 1);
  TrackId second = tracker_->Begin(factory, 2);
  ASSERT_NE(first, second);
  ArgExtractor extractor(storage_->arg_table());
  for (TrackId id : {first, second}) {
    auto rr = storage_->track_table()[id];
    ASSERT_EQ(rr.upid(), 1u);
    ASSERT_TRUE(rr.dimension_arg_set_id());
    for (uint32_t i = 0; i < 25; ++i) {
      std::string key = "custom_dimension.d" + std::to_string(i);
      ASSERT_NE(extractor.Get(*rr.dimension_arg_set_id(), key.c_str()),
                std::numeric_limits<uint32_t>::max());
      EXPECT_EQ(GetArgValue(*storage_, extractor.cursor()),
                Variadic::Integer(i));
    }
  }
}

TEST_F(TrackCompressorUnittest, AdditionalDimensionsPartitionInterning) {
  StringId key = storage_->InternString("custom_dimension.rank");
  ArgsTracker::CompactArg integer{key, key, Variadic::Integer(3)};
  ArgsTracker::CompactArg string{key, key,
                                 Variadic::String(storage_->InternString("3"))};
  constexpr auto bp = tracks::SliceBlueprint("additional");
  auto integer_dimensions = TrackTracker::MakeAdditionalDimensions({integer});
  auto string_dimensions = TrackTracker::MakeAdditionalDimensions({string});
  TrackId first = track_tracker_->InternTrackWithDimensions(
      bp, tracks::Dimensions(), integer_dimensions);
  EXPECT_EQ(first, track_tracker_->InternTrackWithDimensions(
                       bp, tracks::Dimensions(), integer_dimensions));
  EXPECT_NE(first, track_tracker_->InternTrackWithDimensions(
                       bp, tracks::Dimensions(), string_dimensions));
  EXPECT_NE(first, track_tracker_->InternTrack(bp));
}

TEST_F(TrackCompressorUnittest, AdditionalDimensionsNormalizeAndOwnArgs) {
  StringId first_key = storage_->InternString("custom_dimension.first");
  StringId second_key = storage_->InternString("custom_dimension.second");
  ArgsTracker::CompactArg first{first_key, first_key, Variadic::Integer(3)};
  ArgsTracker::CompactArg second{second_key, second_key, Variadic::Integer(4)};
  std::vector<ArgsTracker::CompactArg> source{first, second};
  auto dimensions = TrackTracker::MakeAdditionalDimensions(source);
  auto reordered = TrackTracker::MakeAdditionalDimensions({second, first});
  source[0].value = Variadic::Integer(99);
  EXPECT_EQ(dimensions.hash(), reordered.hash());
  constexpr auto bp = tracks::SliceBlueprint("normalized_additional");
  TrackId id = track_tracker_->InternTrackWithDimensions(
      bp, tracks::Dimensions(), dimensions);
  EXPECT_EQ(id, track_tracker_->InternTrackWithDimensions(
                    bp, tracks::Dimensions(), reordered));
  ArgExtractor extractor(storage_->arg_table());
  ASSERT_NE(extractor.Get(*storage_->track_table()[id].dimension_arg_set_id(),
                          "custom_dimension.first"),
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(GetArgValue(*storage_, extractor.cursor()), Variadic::Integer(3));
}

TEST_F(TrackCompressorUnittest, DisplayLabelsDoNotPartitionLanes) {
  StringId key = storage_->InternString("custom_dimension.rank");
  auto dimensions = TrackTracker::MakeAdditionalDimensions(
      {{key, key, Variadic::Integer(3)}});
  StringId display_key =
      storage_->InternString("custom_dimension_display_name.rank");
  StringId description_key = storage_->InternString("description");
  auto first = tracker_->CreateTrackFactoryWithDimensions(
      kNestable, tracks::Dimensions(1), dimensions, tracks::BlueprintName(),
      [&](ArgsInserter& args) {
        args.AddArg(display_key,
                    Variadic::String(storage_->InternString("east")));
        args.AddArg(description_key,
                    Variadic::String(storage_->InternString("description")));
      });
  auto second = tracker_->CreateTrackFactoryWithDimensions(
      kNestable, tracks::Dimensions(1), dimensions, tracks::BlueprintName(),
      [&](ArgsInserter& args) {
        args.AddArg(display_key,
                    Variadic::String(storage_->InternString("west")));
      });
  TrackId id = tracker_->Scoped(first, 1, 1);
  EXPECT_EQ(id, tracker_->Scoped(second, 2, 1));
  auto rr = storage_->track_table()[id];
  ArgExtractor extractor(storage_->arg_table());
  ASSERT_NE(extractor.Get(*rr.source_arg_set_id(),
                          "custom_dimension_display_name.rank"),
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(GetArgValue(*storage_, extractor.cursor()),
            Variadic::String(storage_->InternString("east")));
  ASSERT_NE(extractor.Get(*rr.source_arg_set_id(), "description"),
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(GetArgValue(*storage_, extractor.cursor()),
            Variadic::String(storage_->InternString("description")));
}

TEST_F(TrackCompressorUnittest, Smoke) {
  auto begin = tracker_->InternBegin(kUnnestable, tracks::Dimensions(1), 1);
  auto end = tracker_->InternEnd(kUnnestable, tracks::Dimensions(1), 1);

  ASSERT_EQ(begin, end);

  const auto& process = storage_->track_table();
  auto rr = process[begin];
  ASSERT_EQ(rr.upid(), 1u);
  ASSERT_EQ(rr.name(), storage_->string_pool().GetId("test"));
}

TEST_F(TrackCompressorUnittest, EndFirst) {
  auto end = tracker_->InternEnd(kNestable, tracks::Dimensions(1), 1);

  const auto& process = storage_->track_table();
  auto rr = process[end];
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

TEST_F(TrackCompressorUnittest, LegacyOverlappingScopedNests) {
  // Same input as |OnlyInternScoped|, but contained slices nest onto one lane.
  TrackId a = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 100, 10);
  TrackId b = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 105, 2);
  TrackId c = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 107, 3);
  TrackId d = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 110, 5);

  ASSERT_EQ(a, b);
  ASSERT_EQ(a, c);
  ASSERT_EQ(a, d);
}

TEST_F(TrackCompressorUnittest, LegacyOverlappingScopedPartialOverlapSpills) {
  // b=[50,150] partially overlaps a=[0,100] so spills; c=[60,70] is contained
  // in a so nests under a, not b.
  TrackId a = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 0, 100);
  TrackId b = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 50, 100);
  TrackId c = tracker_->InternLegacyOverlappingScoped(
      kNestable, tracks::Dimensions(1), 60, 10);

  ASSERT_NE(a, b);
  ASSERT_EQ(a, c);
  ASSERT_NE(b, c);
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
