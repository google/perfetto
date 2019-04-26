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

#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

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

  tracker.Begin(2 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/);
  tracker.End(10 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/);

  auto slices = context.storage->nestable_slices();
  EXPECT_EQ(slices.slice_count(), 1);
  EXPECT_EQ(slices.start_ns()[0], 2);
  EXPECT_EQ(slices.durations()[0], 8);
  EXPECT_EQ(slices.cats()[0], 0);
  EXPECT_EQ(slices.names()[0], 1);
  EXPECT_EQ(slices.refs()[0], 42);
  EXPECT_EQ(slices.types()[0], kRefUtid);
  EXPECT_EQ(slices.depths()[0], 0);
}

TEST(SliceTrackerTest, TwoSliceDetailed) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  tracker.Begin(2 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/);
  tracker.Begin(3 /*ts*/, 42 /*tid*/, 0 /*cat*/, 2 /*name*/);
  tracker.End(5 /*ts*/, 42 /*tid*/);
  tracker.End(10 /*ts*/, 42 /*tid*/);

  auto slices = context.storage->nestable_slices();

  EXPECT_EQ(slices.slice_count(), 2);

  size_t idx = 0;
  EXPECT_EQ(slices.start_ns()[idx], 2);
  EXPECT_EQ(slices.durations()[idx], 8);
  EXPECT_EQ(slices.cats()[idx], 0);
  EXPECT_EQ(slices.names()[idx], 1);
  EXPECT_EQ(slices.refs()[idx], 42);
  EXPECT_EQ(slices.types()[idx], kRefUtid);
  EXPECT_EQ(slices.depths()[idx++], 0);

  EXPECT_EQ(slices.start_ns()[idx], 3);
  EXPECT_EQ(slices.durations()[idx], 2);
  EXPECT_EQ(slices.cats()[idx], 0);
  EXPECT_EQ(slices.names()[idx], 2);
  EXPECT_EQ(slices.refs()[idx], 42);
  EXPECT_EQ(slices.types()[idx], kRefUtid);
  EXPECT_EQ(slices.depths()[idx], 1);

  EXPECT_EQ(slices.parent_stack_ids()[0], 0);
  EXPECT_EQ(slices.stack_ids()[0], slices.parent_stack_ids()[1]);
  EXPECT_NE(slices.stack_ids()[1], 0);
}

TEST(SliceTrackerTest, Scoped) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  tracker.Begin(0 /*ts*/, 42 /*tid*/, 0, 0);
  tracker.Begin(1 /*ts*/, 42 /*tid*/, 0, 0);
  tracker.Scoped(2 /*ts*/, 42 /*tid*/, 0, 0, 6);
  tracker.End(9 /*ts*/, 42 /*tid*/);
  tracker.End(10 /*ts*/, 42 /*tid*/);

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{1, 8}, SliceInfo{2, 6}));
}

TEST(SliceTrackerTest, IgnoreMismatchedEnds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  SliceTracker tracker(&context);

  tracker.Begin(2 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/);
  tracker.End(3 /*ts*/, 42 /*tid*/, 1 /*cat*/, 1 /*name*/);
  tracker.End(4 /*ts*/, 42 /*tid*/, 0 /*cat*/, 2 /*name*/);
  tracker.End(5 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/);

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
  tracker.Scoped(2 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/, 10 /* dur */);
  tracker.Scoped(2 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/, 0 /* dur */);
  tracker.Scoped(12 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/, 1 /* dur */);
  tracker.Scoped(13 /*ts*/, 42 /*tid*/, 0 /*cat*/, 1 /*name*/, 1 /* dur */);

  auto slices = ToSliceInfo(context.storage->nestable_slices());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 10}, SliceInfo{2, 0},
                                  SliceInfo{12, 1}, SliceInfo{13, 1}));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
