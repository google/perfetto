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

#include "src/trace_processor/containers/interval_tree.h"

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

TEST(IntervalTree, Trivial) {
  std::vector<Interval> interval({{10, 20, 5}});
  IntervalTree tree(interval);
  std::vector<uint32_t> overlaps;
  tree.FindOverlaps(5, 30, overlaps);

  ASSERT_THAT(overlaps, UnorderedElementsAre(5));
}

TEST(IntervalTree, Simple) {
  auto intervals = CreateIntervals({{0, 10}, {5, 20}, {30, 40}});
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;
  tree.FindOverlaps(4, 30, overlaps);

  ASSERT_THAT(overlaps, UnorderedElementsAre(0, 1));
}

TEST(IntervalTree, SinglePointOverlap) {
  auto intervals = CreateIntervals({{10, 20}});
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;

  // Overlaps at the start point only
  tree.FindOverlaps(10, 10, overlaps);
  ASSERT_THAT(overlaps, IsEmpty());

  overlaps.clear();

  // Overlaps at the end point only
  tree.FindOverlaps(20, 20, overlaps);
  ASSERT_THAT(overlaps, IsEmpty());
}

TEST(IntervalTree, NoOverlaps) {
  auto intervals = CreateIntervals({{10, 20}, {30, 40}});
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;

  // Before all intervals
  tree.FindOverlaps(5, 9, overlaps);
  ASSERT_THAT(overlaps, IsEmpty());
  overlaps.clear();

  // Between intervals
  tree.FindOverlaps(21, 29, overlaps);
  ASSERT_THAT(overlaps, IsEmpty());
  overlaps.clear();

  // After all intervals
  tree.FindOverlaps(41, 50, overlaps);
  ASSERT_THAT(overlaps, IsEmpty());
}

TEST(IntervalTree, IdenticalIntervals) {
  auto intervals = CreateIntervals({{10, 20}, {10, 20}});
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;
  tree.FindOverlaps(10, 20, overlaps);
  ASSERT_THAT(overlaps, UnorderedElementsAre(0, 1));
}

TEST(IntervalTree, MultipleOverlapsVariousPositions) {
  auto intervals = CreateIntervals({{5, 15}, {10, 20}, {12, 22}, {25, 35}});
  IntervalTree tree(intervals);

  std::vector<uint32_t> overlaps;
  /// Starts before, ends within
  tree.FindOverlaps(9, 11, overlaps);
  ASSERT_THAT(overlaps, UnorderedElementsAre(0, 1));

  overlaps.clear();
  // Starts within, ends within
  tree.FindOverlaps(13, 21, overlaps);
  ASSERT_THAT(overlaps, UnorderedElementsAre(0, 1, 2));

  overlaps.clear();
  // Starts within, ends after
  tree.FindOverlaps(18, 26, overlaps);
  ASSERT_THAT(overlaps, UnorderedElementsAre(1, 2, 3));
}

TEST(IntervalTree, OverlappingEndpoints) {
  auto intervals = CreateIntervals({{10, 20}, {20, 30}});
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;

  tree.FindOverlaps(19, 21, overlaps);
  ASSERT_THAT(overlaps, UnorderedElementsAre(0, 1));
}

TEST(IntervalTree, Stress) {
  static constexpr size_t kCount = 9249;
  std::minstd_rand0 rng(42);

  std::vector<std::pair<uint32_t, uint32_t>> periods;
  uint32_t prev_max = 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    prev_max += static_cast<uint32_t>(rng()) % 100;
    periods.push_back(
        {prev_max, prev_max + (static_cast<uint32_t>(rng()) % 100)});
  }
  auto intervals = CreateIntervals(periods);
  IntervalTree tree(intervals);
  std::vector<uint32_t> overlaps;
  tree.FindOverlaps(periods.front().first, periods.back().first + 1, overlaps);

  EXPECT_EQ(overlaps.size(), kCount);
}

}  // namespace
}  // namespace perfetto::trace_processor
