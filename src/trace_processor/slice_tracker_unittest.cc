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

std::vector<SliceInfo> ToSliceInfo(const TraceStorage::NestableSlices& slices) {
  std::vector<SliceInfo> infos;
  for (size_t i = 0; i < slices.slice_count(); i++) {
    infos.emplace_back(SliceInfo{slices.start_ns()[i], slices.durations()[i]});
  }
  return infos;
}

TEST(SliceTrackerTest, OneSliceDetailed) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track = 22u;
  tracker.Begin(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                1 /*name*/);
  tracker.End(10 /*ts*/, track, 0 /*cat*/, 1 /*name*/);

  auto slices = context.storage->nestable_slices();
  EXPECT_EQ(slices.slice_count(), 1u);
  EXPECT_EQ(slices.start_ns()[0], 2);
  EXPECT_EQ(slices.durations()[0], 8);
  EXPECT_EQ(slices.track_id()[0], track);
  EXPECT_EQ(slices.categories()[0], 0u);
  EXPECT_EQ(slices.names()[0], 1u);
  EXPECT_EQ(slices.refs()[0], 42);
  EXPECT_EQ(slices.types()[0], RefType::kRefUtid);
  EXPECT_EQ(slices.depths()[0], 0);
  EXPECT_EQ(slices.arg_set_ids()[0], kInvalidArgSetId);
}

TEST(SliceTrackerTest, OneSliceWithArgs) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track = 22u;
  tracker.Begin(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                1 /*name*/, [](ArgsTracker* args_tracker, RowId row) {
                  args_tracker->AddArg(row, /*flat_key=*/1, /*key=*/2,
                                       /*value=*/Variadic::Integer(10));
                });
  tracker.End(10 /*ts*/, track, 0 /*cat*/, 1 /*name*/,
              [](ArgsTracker* args_tracker, RowId row) {
                args_tracker->AddArg(row, /*flat_key=*/3, /*key=*/4,
                                     /*value=*/Variadic::Integer(20));
              });

  auto slices = context.storage->nestable_slices();
  EXPECT_EQ(slices.slice_count(), 1u);
  EXPECT_EQ(slices.start_ns()[0], 2);
  EXPECT_EQ(slices.durations()[0], 8);
  EXPECT_EQ(slices.track_id()[0], track);
  EXPECT_EQ(slices.categories()[0], 0u);
  EXPECT_EQ(slices.names()[0], 1u);
  EXPECT_EQ(slices.refs()[0], 42);
  EXPECT_EQ(slices.types()[0], RefType::kRefUtid);
  EXPECT_EQ(slices.depths()[0], 0);
  auto set_id = slices.arg_set_ids()[0];

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

  constexpr TrackId track = 22u;
  tracker.Begin(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                1 /*name*/);
  tracker.Begin(3 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                2 /*name*/);
  tracker.End(5 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  auto slices = context.storage->nestable_slices();

  EXPECT_EQ(slices.slice_count(), 2u);

  size_t idx = 0;
  EXPECT_EQ(slices.start_ns()[idx], 2);
  EXPECT_EQ(slices.durations()[idx], 8);
  EXPECT_EQ(slices.track_id()[idx], track);
  EXPECT_EQ(slices.categories()[idx], 0u);
  EXPECT_EQ(slices.names()[idx], 1u);
  EXPECT_EQ(slices.refs()[idx], 42);
  EXPECT_EQ(slices.types()[idx], RefType::kRefUtid);
  EXPECT_EQ(slices.depths()[idx++], 0);

  EXPECT_EQ(slices.start_ns()[idx], 3);
  EXPECT_EQ(slices.durations()[idx], 2);
  EXPECT_EQ(slices.track_id()[idx], track);
  EXPECT_EQ(slices.categories()[idx], 0u);
  EXPECT_EQ(slices.names()[idx], 2u);
  EXPECT_EQ(slices.refs()[idx], 42);
  EXPECT_EQ(slices.types()[idx], RefType::kRefUtid);
  EXPECT_EQ(slices.depths()[idx], 1);

  EXPECT_EQ(slices.parent_stack_ids()[0], 0);
  EXPECT_EQ(slices.stack_ids()[0], slices.parent_stack_ids()[1]);
  EXPECT_NE(slices.stack_ids()[1], 0);
}

TEST(SliceTrackerTest, Scoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track = 22u;
  tracker.Begin(0 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0, 0);
  tracker.Begin(1 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0, 0);
  tracker.Scoped(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0, 0, 6);
  tracker.End(9 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{1, 8}, SliceInfo{2, 6}));
}

TEST(SliceTrackerTest, IgnoreMismatchedEnds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track = 22u;
  tracker.Begin(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                1 /*name*/);
  tracker.End(3 /*ts*/, track, 1 /*cat*/, 1 /*name*/);
  tracker.End(4 /*ts*/, track, 0 /*cat*/, 2 /*name*/);
  tracker.End(5 /*ts*/, track, 0 /*cat*/, 1 /*name*/);

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 3}));
}

TEST(SliceTrackerTest, ZeroLengthScoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  // Bug scenario: the second zero-length scoped slice prevents the first slice
  // from being closed, leading to an inconsistency when we try to insert the
  // final slice and it doesn't intersect with the still pending first slice.
  constexpr TrackId track = 22u;
  tracker.Scoped(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                 1 /*name*/, 10 /* dur */);
  tracker.Scoped(2 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                 1 /*name*/, 0 /* dur */);
  tracker.Scoped(12 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                 1 /*name*/, 1 /* dur */);
  tracker.Scoped(13 /*ts*/, track, 42 /*ref*/, RefType::kRefUtid, 0 /*cat*/,
                 1 /*name*/, 1 /* dur */);

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 10}, SliceInfo{2, 0},
                                  SliceInfo{12, 1}, SliceInfo{13, 1}));
}

TEST(SliceTrackerTest, DifferentTracks) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  constexpr TrackId track_a = 22u;
  constexpr TrackId track_b = 23u;
  tracker.Begin(0 /*ts*/, track_a, 42 /*ref*/, RefType::kRefUtid, 0, 0);
  tracker.Scoped(2 /*ts*/, track_b, 42 /*ref*/, RefType::kRefUtid, 0, 0, 6);
  tracker.Scoped(3 /*ts*/, track_b, 42 /*ref*/, RefType::kRefUtid, 0, 0, 4);
  tracker.End(10 /*ts*/, track_a);
  tracker.FlushPendingSlices();

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{2, 6}, SliceInfo{3, 4}));

  EXPECT_EQ(context.storage->nestable_slices().track_id()[0], track_a);
  EXPECT_EQ(context.storage->nestable_slices().track_id()[1], track_b);
  EXPECT_EQ(context.storage->nestable_slices().track_id()[2], track_b);
  EXPECT_EQ(context.storage->nestable_slices().depths()[0], 0);
  EXPECT_EQ(context.storage->nestable_slices().depths()[1], 0);
  EXPECT_EQ(context.storage->nestable_slices().depths()[2], 1);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
