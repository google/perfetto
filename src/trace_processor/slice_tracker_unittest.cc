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
  uint64_t start;
  uint64_t duration;

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
  EXPECT_EQ(slices.utids()[0], 42);
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

  EXPECT_EQ(slices.start_ns()[0], 3);
  EXPECT_EQ(slices.durations()[0], 2);
  EXPECT_EQ(slices.cats()[0], 0);
  EXPECT_EQ(slices.names()[0], 2);
  EXPECT_EQ(slices.utids()[0], 42);
  EXPECT_EQ(slices.depths()[0], 1);

  EXPECT_EQ(slices.start_ns()[1], 2);
  EXPECT_EQ(slices.durations()[1], 8);
  EXPECT_EQ(slices.cats()[1], 0);
  EXPECT_EQ(slices.names()[1], 1);
  EXPECT_EQ(slices.utids()[1], 42);
  EXPECT_EQ(slices.depths()[1], 0);

  EXPECT_EQ(slices.parent_stack_ids()[1], 0);
  EXPECT_EQ(slices.stack_ids()[1], slices.parent_stack_ids()[0]);
  EXPECT_NE(slices.stack_ids()[0], 0);
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
              ElementsAre(SliceInfo{2, 6}, SliceInfo{1, 8}, SliceInfo{0, 10}));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
