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

#include "src/trace_processor/core/util/interval_gaps.h"

#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

constexpr Ts kForever = std::numeric_limits<int64_t>::max();

// Each gap as (start, end, id, lane), and clears them.
std::vector<std::tuple<Ts, Ts, uint32_t, uint32_t>> Take(IntervalGaps& gaps) {
  std::vector<std::tuple<Ts, Ts, uint32_t, uint32_t>> out;
  for (const IntervalGaps::Gap& gap : gaps.gaps()) {
    out.emplace_back(gap.start, gap.end, gap.id, gap.lane);
  }
  gaps.gaps().clear();
  return out;
}

TEST(IntervalGapsTest, FillsAroundCoveringIntervals) {
  std::vector<Interval> background = {{0, 100, 7}};
  IntervalGaps gaps;
  gaps.StartLane(0, background);
  gaps.Cover(10, 20);
  gaps.Cover(50, 60);
  gaps.EndLane();
  EXPECT_THAT(Take(gaps),
              ElementsAre(std::make_tuple(Ts{0}, Ts{10}, 7u, 0u),
                          std::make_tuple(Ts{20}, Ts{50}, 7u, 0u),
                          std::make_tuple(Ts{60}, Ts{100}, 7u, 0u)));
}

// Overlapping and nested covering intervals leave no gap between them.
TEST(IntervalGapsTest, OverlappingCoverLeavesNoGap) {
  std::vector<Interval> background = {{0, 100, 0}};
  IntervalGaps gaps;
  gaps.StartLane(0, background);
  gaps.Cover(10, 40);
  gaps.Cover(15, 20);
  gaps.Cover(30, 50);
  gaps.EndLane();
  EXPECT_THAT(Take(gaps),
              ElementsAre(std::make_tuple(Ts{0}, Ts{10}, 0u, 0u),
                          std::make_tuple(Ts{50}, Ts{100}, 0u, 0u)));
}

// A gap never crosses from one background interval into the next.
TEST(IntervalGapsTest, GapsStayWithinBackgroundIntervals) {
  std::vector<Interval> background = {{0, 10, 0}, {20, 30, 1}, {40, 50, 2}};
  IntervalGaps gaps;
  gaps.StartLane(0, background);
  gaps.Cover(5, 25);
  gaps.EndLane();
  EXPECT_THAT(Take(gaps), ElementsAre(std::make_tuple(Ts{0}, Ts{5}, 0u, 0u),
                                      std::make_tuple(Ts{25}, Ts{30}, 1u, 0u),
                                      std::make_tuple(Ts{40}, Ts{50}, 2u, 0u)));
}

// A zero-width interval covers nothing, and one which never ends covers the
// rest of the lane.
TEST(IntervalGapsTest, PointsAndForever) {
  std::vector<Interval> background = {{0, 100, 0}};
  IntervalGaps gaps;
  gaps.StartLane(0, background);
  gaps.Cover(30, 30);
  gaps.Cover(60, kForever);
  gaps.EndLane();
  EXPECT_THAT(Take(gaps), ElementsAre(std::make_tuple(Ts{0}, Ts{60}, 0u, 0u)));
}

// Starting a lane ends the previous one, and a lane with no covering
// intervals is one gap.
TEST(IntervalGapsTest, LanesAreIndependent) {
  std::vector<Interval> first = {{0, 10, 0}};
  std::vector<Interval> second = {{5, 15, 1}};
  IntervalGaps gaps;
  gaps.StartLane(3, first);
  gaps.Cover(0, 4);
  gaps.StartLane(4, second);
  gaps.EndLane();
  gaps.StartLane(5, {});
  gaps.Cover(0, 10);
  gaps.EndLane();
  EXPECT_THAT(Take(gaps), ElementsAre(std::make_tuple(Ts{4}, Ts{10}, 0u, 3u),
                                      std::make_tuple(Ts{5}, Ts{15}, 1u, 4u)));
  EXPECT_THAT(Take(gaps), IsEmpty());
}

}  // namespace
}  // namespace perfetto::trace_processor::core
