/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <vector>

#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;

struct SliceInfo {
  int64_t start;
  int64_t duration;

  bool operator==(const SliceInfo& other) const {
    return std::tie(start, duration) == std::tie(other.start, other.duration);
  }
};

inline void PrintTo(const SliceInfo& info, ::std::ostream* os) {
  *os << "SliceInfo{" << info.start << ", " << info.duration << "}";
}

std::vector<SliceInfo> ToSliceInfo(const tables::SliceTable& slices) {
  std::vector<SliceInfo> infos;
  for (uint32_t i = 0; i < slices.row_count(); i++) {
    infos.emplace_back(SliceInfo{slices.ts()[i], slices.dur()[i]});
  }
  return infos;
}

TEST(SliceTrackerTest, OneSliceDetailed) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, 0 /*cat*/, 1 /*name*/);
  tracker.End(10 /*ts*/, track, 0 /*cat*/, 1 /*name*/);

  const auto& slices = context.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices.ts()[0], 2);
  EXPECT_EQ(slices.dur()[0], 8);
  EXPECT_EQ(slices.track_id()[0], track.value);
  EXPECT_EQ(slices.category()[0], 0u);
  EXPECT_EQ(slices.name()[0], 1u);
  EXPECT_EQ(slices.depth()[0], 0u);
  EXPECT_EQ(slices.arg_set_id()[0], kInvalidArgSetId);
}

TEST(SliceTrackerTest, OneSliceWithArgs) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, 0 /*cat*/, 1 /*name*/,
                [](ArgsTracker::BoundInserter* inserter) {
                  inserter->AddArg(/*flat_key=*/1, /*key=*/2,
                                   /*value=*/Variadic::Integer(10));
                });
  tracker.End(10 /*ts*/, track, 0 /*cat*/, 1 /*name*/,
              [](ArgsTracker::BoundInserter* inserter) {
                inserter->AddArg(/*flat_key=*/3, /*key=*/4,
                                 /*value=*/Variadic::Integer(20));
              });

  const auto& slices = context.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices.ts()[0], 2);
  EXPECT_EQ(slices.dur()[0], 8);
  EXPECT_EQ(slices.track_id()[0], track.value);
  EXPECT_EQ(slices.category()[0], 0u);
  EXPECT_EQ(slices.name()[0], 1u);
  EXPECT_EQ(slices.depth()[0], 0u);
  auto set_id = slices.arg_set_id()[0];

  auto args = context.storage->args();
  EXPECT_EQ(args.set_ids()[0], set_id);
  EXPECT_EQ(args.flat_keys()[0], 1u);
  EXPECT_EQ(args.keys()[0], 2u);
  EXPECT_EQ(args.arg_values()[0], Variadic::Integer(10));
  EXPECT_EQ(args.set_ids()[1], set_id);
  EXPECT_EQ(args.flat_keys()[1], 3u);
  EXPECT_EQ(args.keys()[1], 4u);
  EXPECT_EQ(args.arg_values()[1], Variadic::Integer(20));
}

TEST(SliceTrackerTest, TwoSliceDetailed) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, 0 /*cat*/, 1 /*name*/);
  tracker.Begin(3 /*ts*/, track, 0 /*cat*/, 2 /*name*/);
  tracker.End(5 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  const auto& slices = context.storage->slice_table();

  EXPECT_EQ(slices.row_count(), 2u);

  uint32_t idx = 0;
  EXPECT_EQ(slices.ts()[idx], 2);
  EXPECT_EQ(slices.dur()[idx], 8);
  EXPECT_EQ(slices.track_id()[idx], track.value);
  EXPECT_EQ(slices.category()[idx], 0u);
  EXPECT_EQ(slices.name()[idx], 1u);
  EXPECT_EQ(slices.depth()[idx++], 0u);

  EXPECT_EQ(slices.ts()[idx], 3);
  EXPECT_EQ(slices.dur()[idx], 2);
  EXPECT_EQ(slices.track_id()[idx], track.value);
  EXPECT_EQ(slices.category()[idx], 0u);
  EXPECT_EQ(slices.name()[idx], 2u);
  EXPECT_EQ(slices.depth()[idx], 1u);

  EXPECT_EQ(slices.parent_stack_id()[0], 0);
  EXPECT_EQ(slices.stack_id()[0], slices.parent_stack_id()[1]);
  EXPECT_NE(slices.stack_id()[1], 0);
}

TEST(SliceTrackerTest, Scoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Begin(0 /*ts*/, track, 0, 0);
  tracker.Begin(1 /*ts*/, track, 0, 0);
  tracker.Scoped(2 /*ts*/, track, 0, 0, 6);
  tracker.End(9 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  auto slices = ToSliceInfo(context.storage->slice_table());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{1, 8}, SliceInfo{2, 6}));
}

TEST(SliceTrackerTest, IgnoreMismatchedEnds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, 5 /*cat*/, 1 /*name*/);
  tracker.End(3 /*ts*/, track, 1 /*cat*/, 1 /*name*/);
  tracker.End(4 /*ts*/, track, 0 /*cat*/, 2 /*name*/);
  tracker.End(5 /*ts*/, track, 5 /*cat*/, 1 /*name*/);

  auto slices = ToSliceInfo(context.storage->slice_table());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 3}));
}

TEST(SliceTrackerTest, ZeroLengthScoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  // Bug scenario: the second zero-length scoped slice prevents the first slice
  // from being closed, leading to an inconsistency when we try to insert the
  // final slice and it doesn't intersect with the still pending first slice.
  constexpr TrackId track{22u};
  tracker.Scoped(2 /*ts*/, track, 0 /*cat*/, 1 /*name*/, 10 /* dur */);
  tracker.Scoped(2 /*ts*/, track, 0 /*cat*/, 1 /*name*/, 0 /* dur */);
  tracker.Scoped(12 /*ts*/, track, 0 /*cat*/, 1 /*name*/, 1 /* dur */);
  tracker.Scoped(13 /*ts*/, track, 0 /*cat*/, 1 /*name*/, 1 /* dur */);

  auto slices = ToSliceInfo(context.storage->slice_table());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 10}, SliceInfo{2, 0},
                                  SliceInfo{12, 1}, SliceInfo{13, 1}));
}

TEST(SliceTrackerTest, DifferentTracks) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track_a{22u};
  constexpr TrackId track_b{23u};
  tracker.Begin(0 /*ts*/, track_a, 0, 0);
  tracker.Scoped(2 /*ts*/, track_b, 0, 0, 6);
  tracker.Scoped(3 /*ts*/, track_b, 0, 0, 4);
  tracker.End(10 /*ts*/, track_a);
  tracker.FlushPendingSlices();

  auto slices = ToSliceInfo(context.storage->slice_table());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{2, 6}, SliceInfo{3, 4}));

  EXPECT_EQ(context.storage->slice_table().track_id()[0], track_a.value);
  EXPECT_EQ(context.storage->slice_table().track_id()[1], track_b.value);
  EXPECT_EQ(context.storage->slice_table().track_id()[2], track_b.value);
  EXPECT_EQ(context.storage->slice_table().depth()[0], 0u);
  EXPECT_EQ(context.storage->slice_table().depth()[1], 0u);
  EXPECT_EQ(context.storage->slice_table().depth()[2], 1u);
}

TEST(SliceTrackerTest, EndEventOutOfOrder) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track{22u};
  tracker.Scoped(50 /*ts*/, track, 11 /*cat*/, 21 /*name*/, 100 /*dur*/);
  tracker.Begin(100 /*ts*/, track, 12 /*cat*/, 22 /*name*/);
  tracker.Scoped(450 /*ts*/, track, 12 /*cat*/, 22 /*name*/, 100 /*dur*/);
  tracker.End(500 /*ts*/, track, 12 /*cat*/, 22 /*name*/);

  // This slice should now have depth 0.
  tracker.Begin(800 /*ts*/, track, 13 /*cat*/, 23 /*name*/);
  // Null cat and name matches everything.
  tracker.End(1000 /*ts*/, track, 0 /*cat*/, 0 /*name*/);

  // Slice will not close if category is different.
  tracker.Begin(1100 /*ts*/, track, 11 /*cat*/, 21 /*name*/);
  tracker.End(1200 /*ts*/, track, 12 /*cat*/, 21 /*name*/);

  // Slice will not close if name is different.
  tracker.Begin(1300 /*ts*/, track, 11 /*cat*/, 21 /*name*/);
  tracker.End(1400 /*ts*/, track, 11 /*cat*/, 22 /*name*/);

  tracker.FlushPendingSlices();

  auto slices = ToSliceInfo(context.storage->slice_table());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{50, 100}, SliceInfo{100, 400},
                                  SliceInfo{450, 100}, SliceInfo{800, 200},
                                  SliceInfo{1100, -1}, SliceInfo{1300, 0 - 1}));

  EXPECT_EQ(context.storage->slice_table().depth()[0], 0u);
  EXPECT_EQ(context.storage->slice_table().depth()[1], 1u);
  EXPECT_EQ(context.storage->slice_table().depth()[2], 2u);
  EXPECT_EQ(context.storage->slice_table().depth()[3], 0u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
