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
using testing::ElementsAre;
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

TEST(IntervalIntersector, IntervalTree_InstantIntervals) {
  auto intervals = CreateIntervals({{10, 10}, {20, 20}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kIntervalTree);

  // Test case 1: Overlap with first instant
  std::vector<Id> overlaps;
  intersector.FindOverlaps(5, 15, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));

  // Test case 2: Overlap with second instant
  overlaps.clear();
  intersector.FindOverlaps(15, 25, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(1));

  // Test case 3: Query is an instant
  overlaps.clear();
  intersector.FindOverlaps(10, 10, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
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

TEST(IntervalIntersector, BinarySearch_InstantIntervals) {
  auto intervals = CreateIntervals({{10, 10}, {20, 20}});
  IntervalIntersector intersector(intervals,
                                  IntervalIntersector::kBinarySearch);

  // Test case 1: Overlap with first instant
  std::vector<Id> overlaps;
  intersector.FindOverlaps(5, 15, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));

  // Test case 2: Overlap with second instant
  overlaps.clear();
  intersector.FindOverlaps(15, 25, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(1));

  // Test case 3: Query is an instant
  overlaps.clear();
  intersector.FindOverlaps(10, 10, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
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
TEST(IntervalIntersector, OverlapTests) {
  auto intervals = CreateIntervals({{10, 20}, {30, 40}, {15, 25}});
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);

  // Test case 1: No overlap
  std::vector<Id> overlaps;
  intersector.FindOverlaps(0, 5, overlaps);
  EXPECT_THAT(overlaps, IsEmpty());

  // Test case 2: Single overlap
  overlaps.clear();
  intersector.FindOverlaps(18, 22, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 2));

  // Test case 3: Multiple overlaps
  overlaps.clear();
  intersector.FindOverlaps(12, 35, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 1, 2));

  // Test case 4: Query is an instant
  overlaps.clear();
  intersector.FindOverlaps(17, 17, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0, 2));
}

TEST(IntervalIntersector, InstantIntervals) {
  auto intervals = CreateIntervals({{10, 10}, {20, 20}});
  IntervalIntersector intersector(intervals, IntervalIntersector::kLinearScan);

  // Test case 1: Overlap with first instant
  std::vector<Id> overlaps;
  intersector.FindOverlaps(5, 15, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));

  // Test case 2: Overlap with second instant
  overlaps.clear();
  intersector.FindOverlaps(15, 25, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(1));

  // Test case 3: Query is an instant
  overlaps.clear();
  intersector.FindOverlaps(10, 10, overlaps);
  EXPECT_THAT(overlaps, UnorderedElementsAre(0));
}

using Result = std::tuple<Ts, Ts, std::vector<uint32_t>>;

std::vector<Result> IntersectAll(
    const std::vector<const std::vector<Interval>*>& tables) {
  std::vector<Result> results;
  IntervalIntersector::IntersectNonOverlapping(
      tables, [&](Ts start, Ts end, const uint32_t* ids) {
        results.emplace_back(start, end,
                             std::vector<uint32_t>(ids, ids + tables.size()));
      });
  return results;
}

TEST(IntervalIntersector, IntersectNonOverlapping_TwoTables) {
  std::vector<Interval> t0 = {{0, 10, 0}, {20, 30, 1}};
  std::vector<Interval> t1 = {{5, 25, 10}};

  EXPECT_THAT(IntersectAll({&t0, &t1}),
              ElementsAre(Result{5, 10, {0, 10}}, Result{20, 25, {1, 10}}));
}

TEST(IntervalIntersector, IntersectNonOverlapping_ThreeTables) {
  std::vector<Interval> t0 = {{0, 100, 0}};
  std::vector<Interval> t1 = {{10, 60, 1}, {70, 90, 2}};
  std::vector<Interval> t2 = {{20, 50, 3}, {80, 85, 4}};

  EXPECT_THAT(
      IntersectAll({&t0, &t1, &t2}),
      ElementsAre(Result{20, 50, {0, 1, 3}}, Result{80, 85, {0, 2, 4}}));
}

TEST(IntervalIntersector, IntersectNonOverlapping_Instants) {
  std::vector<Interval> t0 = {{10, 10, 0}, {20, 30, 1}};
  std::vector<Interval> t1 = {{0, 10, 2}, {10, 20, 3}, {25, 25, 4}};

  EXPECT_THAT(IntersectAll({&t0, &t1}),
              ElementsAre(Result{10, 10, {0, 3}}, Result{25, 25, {1, 4}}));
}

TEST(IntervalIntersector, IntersectNonOverlapping_EmptyTable) {
  std::vector<Interval> t0 = {{0, 10, 0}};
  std::vector<Interval> t1;

  EXPECT_THAT(IntersectAll({&t0, &t1}), IsEmpty());
}

}  // namespace
}  // namespace perfetto::trace_processor
