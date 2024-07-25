/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/containers/interval_intersector.h"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

inline bool operator==(const Interval& a, const Interval& b) {
  return std::tie(a.start, a.end, a.id) == std::tie(b.start, b.end, b.id);
}

namespace {

using Interval = Interval;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

std::vector<Interval> CreateIntervals(
    std::vector<std::pair<uint32_t, uint32_t>> periods) {
  std::vector<Interval> res;
  uint32_t id = 0;
  for (auto period : periods) {
    res.push_back({period.first, period.second, id++});
  }
  return res;
}

TEST(IntervalIntersector, IntervalTree_EmptyInput) {
  std::vector<Interval> intervals;
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kIntervalTree);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 10, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

TEST(IntervalIntersector, IntervalTree_SingleIntervalFullOverlap) {
  auto intervals = CreateIntervals({{5, 15}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kIntervalTree);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 20, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
}

TEST(IntervalIntersector, IntervalTree_MultipleOverlaps) {
  auto intervals = CreateIntervals({{0, 10}, {5, 15}, {20, 30}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kIntervalTree);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(8, 25, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 1, 2));
}

TEST(IntervalIntersector, IntervalTree_NoOverlap) {
  auto intervals = CreateIntervals({{0, 5}, {10, 15}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kIntervalTree);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(6, 9, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

// Tests for kBinarySearch Mode

TEST(IntervalIntersector, BinarySearch_EmptyInput) {
  std::vector<Interval> intervals;
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kBinarySearch);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 10, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

TEST(IntervalIntersector, BinarySearch_SingleIntervalFullOverlap) {
  auto intervals = CreateIntervals({{5, 15}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kBinarySearch);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 20, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
}

TEST(IntervalIntersector, BinarySearch_MultipleOverlaps) {
  auto intervals =
      CreateIntervals({{0, 5}, {10, 15}, {20, 25}});  // Non-overlapping
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kBinarySearch);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(3, 22, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 1, 2));
}

TEST(IntervalIntersector, BinarySearch_NoOverlap) {
  auto intervals = CreateIntervals({{0, 5}, {10, 15}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kBinarySearch);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(6, 9, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

// Tests for kLinearScan Mode

TEST(IntervalIntersector, LinearScan_EmptyInput) {
  std::vector<Interval> intervals;
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 10, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

TEST(IntervalIntersector, LinearScan_SingleIntervalFullOverlap) {
  auto intervals = CreateIntervals({{5, 15}});
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 20, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
}

TEST(IntervalIntersector, LinearScan_MultipleOverlaps) {
  auto intervals = CreateIntervals({{0, 10}, {5, 15}, {20, 30}});
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(8, 25, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 1, 2));
}

TEST(IntervalIntersector, LinearScan_NoOverlap) {
  auto intervals = CreateIntervals({{0, 5}, {10, 15}});
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);
  std::vector<Id> overlaps;
  intersector.FindOverlaps(6, 9, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());
}

}  // namespace
}  // namespace perfetto::trace_processor
