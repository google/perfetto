/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/exec/interval_index.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

struct Interval {
  int64_t key;
  int64_t start;
  int64_t end;
};

// An index over `intervals`, each handed back as its position in the vector.
IntervalIndex Index(const std::vector<Interval>& intervals) {
  IntervalIndex index;
  for (uint32_t i = 0; i < intervals.size(); ++i) {
    index.Add(&intervals[i].key, 1, intervals[i].start, intervals[i].end, i);
  }
  index.Build();
  return index;
}

// The rows the sweep holds active, in the order it holds them.
std::vector<uint32_t> Active(const IntervalIndex& index,
                             const IntervalIndex::Sweep& sweep) {
  std::vector<uint32_t> rows;
  for (uint32_t i : sweep.active()) {
    rows.push_back(index.row(i));
  }
  return rows;
}

// The rows of `intervals` in `key`'s lane containing `s`, by start then by
// position: what active() must hold after advancing to `s`.
std::vector<uint32_t> Containing(const std::vector<Interval>& intervals,
                                 int64_t key,
                                 int64_t s) {
  std::vector<uint32_t> rows;
  for (uint32_t i = 0; i < intervals.size(); ++i) {
    const Interval& in = intervals[i];
    bool contains =
        in.start == in.end ? in.start == s : in.start <= s && s < in.end;
    if (in.key == key && contains) {
      rows.push_back(i);
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [&](uint32_t a, uint32_t b) {
    return intervals[a].start < intervals[b].start;
  });
  return rows;
}

// The row of the first interval of `key`'s lane starting after `s`, or
// nothing when none does: what next() must point at.
std::optional<uint32_t> NextAfter(const std::vector<Interval>& intervals,
                                  int64_t key,
                                  int64_t s) {
  std::optional<uint32_t> best;
  for (uint32_t i = 0; i < intervals.size(); ++i) {
    const Interval& in = intervals[i];
    if (in.key != key || in.start <= s) {
      continue;
    }
    if (!best || in.start < intervals[*best].start ||
        (in.start == intervals[*best].start && i < *best)) {
      best = i;
    }
  }
  return best;
}

std::optional<uint32_t> NextRow(const IntervalIndex& index,
                                const IntervalIndex::Sweep& sweep) {
  if (sweep.next() == sweep.limit()) {
    return std::nullopt;
  }
  return index.row(sweep.next());
}

// Overlapping intervals in lane 1: [0,10) [2,4) [5,20) [8,9), the point 8,
// [30,40); and a disjoint lane 2: [0,5) [5,5) [5,8) [10,12).
const std::vector<Interval> kIntervals = {
    {1, 0, 10},  {1, 2, 4}, {1, 5, 20}, {1, 8, 9}, {1, 8, 8},
    {1, 30, 40}, {2, 0, 5}, {2, 5, 5},  {2, 5, 8}, {2, 10, 12},
};

TEST(IntervalIndex, LanesAreFoundByKey) {
  IntervalIndex index = Index(kIntervals);
  int64_t key = 1;
  EXPECT_TRUE(index.FindLane(&key, 1).has_value());
  key = 3;
  EXPECT_FALSE(index.FindLane(&key, 1).has_value());
  int64_t wide[] = {1, 2};
  IntervalIndex two;
  two.Add(wide, 2, 0, 1, 0);
  two.Build();
  EXPECT_TRUE(two.FindLane(wide, 2).has_value());
  wide[1] = 3;
  EXPECT_FALSE(two.FindLane(wide, 2).has_value());
}

TEST(IntervalIndex, ActiveIsWhatContainsTheProbeInLaneOrder) {
  IntervalIndex index = Index(kIntervals);
  int64_t key = 1;
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));

  ASSERT_TRUE(sweep.Advance(3));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 1));
  EXPECT_EQ(NextRow(index, sweep), 2u);
  // A point is contained by the intervals over it and by an equal point.
  ASSERT_TRUE(sweep.Advance(8));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 2, 3, 4));
  EXPECT_EQ(NextRow(index, sweep), 5u);
  // An end is exclusive.
  ASSERT_TRUE(sweep.Advance(9));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 2));
  ASSERT_TRUE(sweep.Advance(25));
  EXPECT_THAT(Active(index, sweep), IsEmpty());
  EXPECT_EQ(NextRow(index, sweep), 5u);
  ASSERT_TRUE(sweep.Advance(40));
  EXPECT_THAT(Active(index, sweep), IsEmpty());
  EXPECT_EQ(NextRow(index, sweep), std::nullopt);
}

TEST(IntervalIndex, ADisjointLaneHoldsPointsAtAnIntervalsStart) {
  IntervalIndex index = Index(kIntervals);
  int64_t key = 2;
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));
  // [0,5) has ended at 5; the point 5 and [5,8) both contain it.
  ASSERT_TRUE(sweep.Advance(5));
  EXPECT_THAT(Active(index, sweep), ElementsAre(7, 8));
  EXPECT_EQ(NextRow(index, sweep), 9u);
  ASSERT_TRUE(sweep.Advance(6));
  EXPECT_THAT(Active(index, sweep), ElementsAre(8));
  ASSERT_TRUE(sweep.Advance(11));
  EXPECT_THAT(Active(index, sweep), ElementsAre(9));
}

TEST(IntervalIndex, EqualStartsKeepTheirOrder) {
  IntervalIndex index = Index({{0, 5, 9}, {0, 5, 6}, {0, 5, 7}, {0, 1, 3}});
  int64_t key = 0;
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(5));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 1, 2));
}

TEST(IntervalIndex, OneSweepServesLaneAfterLane) {
  IntervalIndex index = Index(kIntervals);
  IntervalIndex::Sweep sweep(index);
  int64_t key = 1;
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(8));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 2, 3, 4));
  // Starting another lane forgets the first, including where it had got to.
  key = 2;
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(1));
  EXPECT_THAT(Active(index, sweep), ElementsAre(6));
  key = 1;
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(1));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0));
}

TEST(IntervalIndex, AProbeBeforeThePreviousOneIsRefused) {
  IntervalIndex index = Index(kIntervals);
  int64_t key = 1;
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(8));
  EXPECT_FALSE(sweep.Advance(7));
  // Nothing changed.
  EXPECT_THAT(Active(index, sweep), ElementsAre(0, 2, 3, 4));
  EXPECT_TRUE(sweep.Advance(8));
}

TEST(IntervalIndex, NeverEndingIntervalsStayActive) {
  constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
  IntervalIndex index = Index({{0, 0, kNever}, {0, 5, 6}});
  int64_t key = 0;
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(kNever - 1));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0));
  EXPECT_EQ(NextRow(index, sweep), std::nullopt);
}

TEST(IntervalIndex, ClearForgetsEverything) {
  IntervalIndex index = Index(kIntervals);
  index.Clear();
  int64_t key = 1;
  EXPECT_FALSE(index.FindLane(&key, 1).has_value());
  index.Add(&key, 1, 0, 1, 0);
  index.Build();
  IntervalIndex::Sweep sweep(index);
  sweep.Start(*index.FindLane(&key, 1));
  ASSERT_TRUE(sweep.Advance(0));
  EXPECT_THAT(Active(index, sweep), ElementsAre(0));
}

// `count` intervals in `lanes` lanes, disjoint within a lane when asked,
// dense enough in time that probes land on starts and ends often.
std::vector<Interval> RandomIntervals(std::minstd_rand& rng,
                                      uint32_t count,
                                      int64_t lanes,
                                      bool disjoint) {
  std::vector<Interval> intervals;
  std::vector<int64_t> lane_end(static_cast<size_t>(lanes), 0);
  for (uint32_t i = 0; i < count; ++i) {
    auto key = static_cast<int64_t>(rng() % static_cast<uint64_t>(lanes));
    int64_t dur = rng() % 4 == 0 ? 0 : static_cast<int64_t>(rng() % 40);
    int64_t start;
    if (disjoint) {
      start =
          lane_end[static_cast<size_t>(key)] + static_cast<int64_t>(rng() % 3);
      lane_end[static_cast<size_t>(key)] = start + dur;
    } else {
      start = static_cast<int64_t>(rng() % 500);
    }
    intervals.push_back({key, start, start + dur});
  }
  return intervals;
}

// Every path of Advance against the definitions: the first probe of a lane,
// probes a step apart which are walked to, and probes far apart which are
// galloped to and re-seeded, on lanes with and without a tree.
TEST(IntervalIndex, SweepMatchesTheDefinitionsOnRandomLanes) {
  std::minstd_rand rng(7);
  for (bool disjoint : {true, false}) {
    for (int64_t stride : {int64_t{1}, int64_t{7}, int64_t{60}, int64_t{400}}) {
      std::vector<Interval> intervals = RandomIntervals(rng, 3000, 5, disjoint);
      IntervalIndex index = Index(intervals);
      IntervalIndex::Sweep sweep(index);
      for (int64_t key = 0; key < 5; ++key) {
        sweep.Start(*index.FindLane(&key, 1));
        int64_t s = static_cast<int64_t>(rng() % 50);
        while (s < 1200) {
          ASSERT_TRUE(sweep.Advance(s));
          ASSERT_EQ(Active(index, sweep), Containing(intervals, key, s))
              << "disjoint " << disjoint << " stride " << stride << " key "
              << key << " at " << s;
          ASSERT_EQ(NextRow(index, sweep), NextAfter(intervals, key, s));
          // Sometimes the same probe again, sometimes a leap.
          s += static_cast<int64_t>(rng() % 3 == 0 ? 0 : rng() % (stride + 1));
        }
      }
    }
  }
}

// The rows meeting `[s, e)`, which are those holding its start followed by
// those starting before it ends: what an interval intersect asks of a lane.
std::vector<uint32_t> Overlaps(const IntervalIndex& index,
                               IntervalIndex::Sweep& sweep,
                               int64_t s,
                               int64_t e) {
  std::vector<uint32_t> rows;
  sweep.Start(0);
  sweep.Advance(s);
  for (uint32_t i : sweep.active()) {
    rows.push_back(index.row(i));
  }
  for (uint32_t i = sweep.next(); i < sweep.limit() && index.start(i) < e;
       ++i) {
    rows.push_back(index.row(i));
  }
  return rows;
}

// An index over `intervals` in one lane, each handed back as its position.
IntervalIndex OneLane(const std::vector<std::pair<int64_t, int64_t>>& ivals) {
  IntervalIndex index;
  for (uint32_t i = 0; i < ivals.size(); ++i) {
    index.Add(nullptr, 0, ivals[i].first, ivals[i].second, i);
  }
  index.Build();
  return index;
}

// An interval holds the instants from its start up to but not including its
// end, so two which touch end to start share none.
TEST(IntervalIndex, IntervalsTouchingEndToStartDoNotMeet) {
  IntervalIndex index = OneLane({{10, 20}, {20, 30}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 19, 21), ElementsAre(0, 1));
  EXPECT_THAT(Overlaps(index, sweep, 20, 21), ElementsAre(1));
  EXPECT_THAT(Overlaps(index, sweep, 5, 10), IsEmpty());
}

// A point meets whatever covers the instant it sits at, which an interval
// ending there does not.
TEST(IntervalIndex, APointMeetsWhatCoversItsInstant) {
  IntervalIndex index = OneLane({{0, 15}, {10, 20}, {15, 15}, {15, 25}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 15, 15), ElementsAre(1, 2, 3));
}

// A range meets the points from its start up to but not including its end.
TEST(IntervalIndex, ARangeMeetsThePointsItHolds) {
  IntervalIndex index = OneLane({{10, 10}, {12, 18}, {15, 15}, {20, 20}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 10, 20), ElementsAre(0, 1, 2));
  EXPECT_THAT(Overlaps(index, sweep, 14, 16), ElementsAre(1, 2));
}

// A point at a range's end is outside it; one at its start is not.
TEST(IntervalIndex, APointAtARangesEndIsOutsideIt) {
  IntervalIndex index = OneLane({{10, 10}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 5, 10), IsEmpty());
  EXPECT_THAT(Overlaps(index, sweep, 10, 15), ElementsAre(0));
  EXPECT_THAT(Overlaps(index, sweep, 10, 10), ElementsAre(0));
  EXPECT_THAT(Overlaps(index, sweep, 9, 11), ElementsAre(0));
}

// Points at one instant are all met together.
TEST(IntervalIndex, PointsAtOneInstantAreMetTogether) {
  IntervalIndex index = OneLane({{10, 10}, {10, 10}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 9, 11), ElementsAre(0, 1));
  EXPECT_THAT(Overlaps(index, sweep, 10, 10), ElementsAre(0, 1));
}

// Intervals over the same instants are all met.
TEST(IntervalIndex, IdenticalIntervalsAreAllMet) {
  IntervalIndex index = OneLane({{10, 20}, {10, 20}, {10, 20}});
  IntervalIndex::Sweep sweep(index);
  EXPECT_THAT(Overlaps(index, sweep, 12, 13), ElementsAre(0, 1, 2));
}

// The instants two intervals share, against the rule written out.
TEST(IntervalIndex, OverlapsMatchTheDefinitionOnRandomLanes) {
  std::minstd_rand rng(7);
  std::vector<std::pair<int64_t, int64_t>> ivals;
  for (uint32_t i = 0; i < 400; ++i) {
    auto start = static_cast<int64_t>(rng() % 200);
    // A quarter of them are points.
    auto len = rng() % 4 == 0 ? 0 : static_cast<int64_t>(rng() % 40);
    ivals.emplace_back(start, start + len);
  }
  IntervalIndex index = OneLane(ivals);
  IntervalIndex::Sweep sweep(index);
  for (uint32_t q = 0; q < 300; ++q) {
    auto s = static_cast<int64_t>(rng() % 220);
    auto e = s + (rng() % 4 == 0 ? 0 : static_cast<int64_t>(rng() % 30));
    std::vector<uint32_t> expected;
    for (uint32_t i = 0; i < ivals.size(); ++i) {
      int64_t a = ivals[i].first;
      int64_t b = ivals[i].second;
      bool meets = s == e ? (a == b ? a == s : a <= s && s < b)
                          : (a == b ? s <= a && a < e : e > a && s < b);
      if (meets) {
        expected.push_back(i);
      }
    }
    std::vector<uint32_t> got = Overlaps(index, sweep, s, e);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected) << "probe [" << s << ", " << e << ")";
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
