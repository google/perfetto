/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/common/clock_tracker.h"

#include <optional>
#include <random>

#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"

namespace perfetto {
namespace trace_processor {

class ClockTrackerTest : public ::testing::Test {
 public:
  ClockTrackerTest() {
    context_.storage.reset(new TraceStorage());
    context_.metadata_tracker.reset(
        new MetadataTracker(context_.storage.get()));
  }

  // using ClockId = uint64_t;
  TraceProcessorContext context_;
  ClockTracker ct_{&context_};
  base::StatusOr<int64_t> Convert(ClockTracker::ClockId src_clock_id,
                                  int64_t src_timestamp,
                                  ClockTracker::ClockId target_clock_id) {
    return ct_.Convert(src_clock_id, src_timestamp, target_clock_id);
  }
};

namespace {

using ::testing::NiceMock;
using Clock = protos::pbzero::ClockSnapshot::Clock;

constexpr auto REALTIME = protos::pbzero::BUILTIN_CLOCK_REALTIME;
constexpr auto BOOTTIME = protos::pbzero::BUILTIN_CLOCK_BOOTTIME;
constexpr auto MONOTONIC = protos::pbzero::BUILTIN_CLOCK_MONOTONIC;
constexpr auto MONOTONIC_COARSE =
    protos::pbzero::BUILTIN_CLOCK_MONOTONIC_COARSE;
constexpr auto MONOTONIC_RAW = protos::pbzero::BUILTIN_CLOCK_MONOTONIC_RAW;

TEST_F(ClockTrackerTest, ClockDomainConversions) {
  EXPECT_FALSE(ct_.ToTraceTime(REALTIME, 0).ok());

  ct_.AddSnapshot({{REALTIME, 10}, {BOOTTIME, 10010}});
  ct_.AddSnapshot({{REALTIME, 20}, {BOOTTIME, 20220}});
  ct_.AddSnapshot({{REALTIME, 30}, {BOOTTIME, 30030}});
  ct_.AddSnapshot({{MONOTONIC, 1000}, {BOOTTIME, 100000}});

  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 0), 10000);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 1), 10001);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 9), 10009);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 10), 10010);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 11), 10011);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 19), 10019);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 20), 20220);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 21), 20221);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 29), 20229);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 30), 30030);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 40), 30040);

  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 0), 100000 - 1000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 999), 100000 - 1);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1000), 100000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1e6),
            static_cast<int64_t>(100000 - 1000 + 1e6));
}

TEST_F(ClockTrackerTest, ToTraceTimeFromSnapshot) {
  EXPECT_FALSE(ct_.ToTraceTime(REALTIME, 0).ok());

  EXPECT_EQ(*ct_.ToTraceTimeFromSnapshot({{REALTIME, 10}, {BOOTTIME, 10010}}),
            10010);
  EXPECT_EQ(ct_.ToTraceTimeFromSnapshot({{MONOTONIC, 10}, {REALTIME, 10010}}),
            std::nullopt);
}

// When a clock moves backwards conversions *from* that clock are forbidden
// but conversions *to* that clock should still work.
// Think to the case of REALTIME going backwards from 3AM to 2AM during DST day.
// You can't convert 2.10AM REALTIME to BOOTTIME because there are two possible
// answers, but you can still unambiguosly convert BOOTTIME into REALTIME.
TEST_F(ClockTrackerTest, RealTimeClockMovingBackwards) {
  ct_.AddSnapshot({{BOOTTIME, 10010}, {REALTIME, 10}});

  // At this point conversions are still possible in both ways because we
  // haven't broken monotonicity yet.
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 11), 10011);

  ct_.AddSnapshot({{BOOTTIME, 10020}, {REALTIME, 20}});
  ct_.AddSnapshot({{BOOTTIME, 30040}, {REALTIME, 40}});
  ct_.AddSnapshot({{BOOTTIME, 40030}, {REALTIME, 30}});

  // Now only BOOTIME -> REALTIME conversion should be possible.
  EXPECT_FALSE(ct_.ToTraceTime(REALTIME, 11).ok());
  EXPECT_EQ(*Convert(BOOTTIME, 10011, REALTIME), 11);
  EXPECT_EQ(*Convert(BOOTTIME, 10029, REALTIME), 29);
  EXPECT_EQ(*Convert(BOOTTIME, 40030, REALTIME), 30);
  EXPECT_EQ(*Convert(BOOTTIME, 40040, REALTIME), 40);

  ct_.AddSnapshot({{BOOTTIME, 50000}, {REALTIME, 50}});
  EXPECT_EQ(*Convert(BOOTTIME, 50005, REALTIME), 55);

  ct_.AddSnapshot({{BOOTTIME, 60020}, {REALTIME, 20}});
  EXPECT_EQ(*Convert(BOOTTIME, 60020, REALTIME), 20);
}

// Simulate the following scenario:
// MONOTONIC = MONOTONIC_COARSE + 10
// BOOTTIME = MONOTONIC + 1000 (until T=200)
// BOOTTIME = MONOTONIC + 2000 (from T=200)
// Then resolve MONOTONIC_COARSE. This requires a two-level resolution:
// MONOTONIC_COARSE -> MONOTONIC -> BOOTTIME.
TEST_F(ClockTrackerTest, ChainedResolutionSimple) {
  ct_.AddSnapshot({{MONOTONIC_COARSE, 1}, {MONOTONIC, 11}});
  ct_.AddSnapshot({{MONOTONIC, 100}, {BOOTTIME, 1100}});
  ct_.AddSnapshot({{MONOTONIC, 200}, {BOOTTIME, 2200}});

  // MONOTONIC_COARSE@100 == MONOTONIC@110 == BOOTTIME@1100.
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 110), 1110);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC_COARSE, 100), 100 + 10 + 1000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC_COARSE, 202), 202 + 10 + 2000);
}

TEST_F(ClockTrackerTest, ChainedResolutionHard) {
  // MONOTONIC_COARSE = MONOTONIC_RAW - 1.
  ct_.AddSnapshot({{MONOTONIC_RAW, 10}, {MONOTONIC_COARSE, 9}});

  // MONOTONIC = MONOTONIC_COARSE - 50.
  ct_.AddSnapshot({{MONOTONIC_COARSE, 100}, {MONOTONIC, 50}});

  // BOOTTIME = MONOTONIC + 1000 until T=100 (see below).
  ct_.AddSnapshot({{MONOTONIC, 1}, {BOOTTIME, 1001}, {REALTIME, 10001}});

  // BOOTTIME = MONOTONIC + 2000 from T=100.
  // At the same time, REALTIME goes backwards.
  ct_.AddSnapshot({{MONOTONIC, 101}, {BOOTTIME, 2101}, {REALTIME, 9101}});

  // 1-hop conversions.
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 2, MONOTONIC_COARSE), 1);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 1, MONOTONIC_RAW), 2);
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 100001, MONOTONIC_COARSE), 100000);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 100000, MONOTONIC_RAW), 100001);

  // 2-hop conversions (MONOTONIC_RAW <-> MONOTONIC_COARSE <-> MONOTONIC).
  // From above, MONOTONIC = (MONOTONIC_RAW - 1) - 50.
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 53, MONOTONIC), 53 - 1 - 50);
  EXPECT_EQ(*Convert(MONOTONIC, 2, MONOTONIC_RAW), 2 + 1 + 50);

  // 3-hop conversions (as above + BOOTTIME)
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 53, BOOTTIME), 53 - 1 - 50 + 1000);
  EXPECT_EQ(*Convert(BOOTTIME, 1002, MONOTONIC_RAW), 1002 - 1000 + 1 + 50);

  EXPECT_EQ(*Convert(MONOTONIC_RAW, 753, BOOTTIME), 753 - 1 - 50 + 2000);
  EXPECT_EQ(*Convert(BOOTTIME, 2702, MONOTONIC_RAW), 2702 - 2000 + 1 + 50);

  // 3-hop conversion to REALTIME, one way only (REALTIME goes backwards).
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 53, REALTIME), 53 - 1 - 50 + 10000);
  EXPECT_EQ(*Convert(MONOTONIC_RAW, 753, REALTIME), 753 - 1 - 50 + 9000);
}

// Regression test for b/158182858. When taking two snapshots back-to-back,
// MONOTONIC_COARSE might be stuck to the last value. We should still be able
// to convert both ways in this case.
TEST_F(ClockTrackerTest, NonStrictlyMonotonic) {
  ct_.AddSnapshot({{BOOTTIME, 101}, {MONOTONIC, 51}, {MONOTONIC_COARSE, 50}});
  ct_.AddSnapshot({{BOOTTIME, 105}, {MONOTONIC, 55}, {MONOTONIC_COARSE, 50}});

  // This last snapshot is deliberately identical to the previous one. This
  // is to simulate the case of taking two snapshots so close to each other
  // that all clocks are identical.
  ct_.AddSnapshot({{BOOTTIME, 105}, {MONOTONIC, 55}, {MONOTONIC_COARSE, 50}});

  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 49, MONOTONIC), 50);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 50, MONOTONIC), 55);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 51, MONOTONIC), 56);

  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 40, BOOTTIME), 91);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 50, BOOTTIME), 105);
  EXPECT_EQ(*Convert(MONOTONIC_COARSE, 55, BOOTTIME), 110);

  EXPECT_EQ(*Convert(BOOTTIME, 91, MONOTONIC_COARSE), 40);
  EXPECT_EQ(*Convert(BOOTTIME, 105, MONOTONIC_COARSE), 50);
  EXPECT_EQ(*Convert(BOOTTIME, 110, MONOTONIC_COARSE), 55);
}

TEST_F(ClockTrackerTest, SequenceScopedClocks) {
  ct_.AddSnapshot({{MONOTONIC, 1000}, {BOOTTIME, 100000}});

  ClockTracker::ClockId c64_1 = ct_.SequenceToGlobalClock(1, 64);
  ClockTracker::ClockId c65_1 = ct_.SequenceToGlobalClock(1, 65);
  ClockTracker::ClockId c66_1 = ct_.SequenceToGlobalClock(1, 66);
  ClockTracker::ClockId c66_2 = ct_.SequenceToGlobalClock(2, 64);

  ct_.AddSnapshot(
      {{MONOTONIC, 10000},
       {c64_1, 100000},
       {c65_1, 100, /*unit_multiplier_ns=*/1000, /*is_incremental=*/false},
       {c66_1, 10, /*unit_multiplier_ns=*/1000, /*is_incremental=*/true}});

  // c64_1 is non-incremental and in nanos.
  EXPECT_EQ(*Convert(c64_1, 150000, MONOTONIC), 60000);
  EXPECT_EQ(*Convert(c64_1, 150000, BOOTTIME), 159000);
  EXPECT_EQ(*ct_.ToTraceTime(c64_1, 150000), 159000);

  // c65_1 is non-incremental and in micros.
  EXPECT_EQ(*Convert(c65_1, 150, MONOTONIC), 60000);
  EXPECT_EQ(*Convert(c65_1, 150, BOOTTIME), 159000);
  EXPECT_EQ(*ct_.ToTraceTime(c65_1, 150), 159000);

  // c66_1 is incremental and in micros.
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 11 */, MONOTONIC), 11000);
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 12 */, MONOTONIC), 12000);
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 13 */, BOOTTIME), 112000);
  EXPECT_EQ(*ct_.ToTraceTime(c66_1, 2 /* abs 15 */), 114000);

  ct_.AddSnapshot(
      {{MONOTONIC, 20000},
       {c66_1, 20, /*unit_multiplier_ns=*/1000, /*incremental=*/true}});
  ct_.AddSnapshot(
      {{MONOTONIC, 20000},
       {c66_2, 20, /*unit_multiplier_ns=*/1000, /*incremental=*/true}});

  // c66_1 and c66_2 are both incremental and in micros, but shouldn't affect
  // each other.
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 21 */, MONOTONIC), 21000);
  EXPECT_EQ(*Convert(c66_2, 2 /* abs 22 */, MONOTONIC), 22000);
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 22 */, MONOTONIC), 22000);
  EXPECT_EQ(*Convert(c66_2, 2 /* abs 24 */, MONOTONIC), 24000);
  EXPECT_EQ(*Convert(c66_1, 1 /* abs 23 */, BOOTTIME), 122000);
  EXPECT_EQ(*Convert(c66_2, 2 /* abs 26 */, BOOTTIME), 125000);
  EXPECT_EQ(*ct_.ToTraceTime(c66_1, 2 /* abs 25 */), 124000);
  EXPECT_EQ(*ct_.ToTraceTime(c66_2, 4 /* abs 30 */), 129000);
}

// Tests that the cache doesn't affect the results of Convert() in unexpected
// ways.
TEST_F(ClockTrackerTest, CacheDoesntAffectResults) {
  std::minstd_rand rnd;
  int last_mono = 0;
  int last_boot = 0;
  int last_raw = 0;
  static const int increments[] = {1, 2, 10};
  for (int i = 0; i < 1000; i++) {
    last_mono += increments[rnd() % base::ArraySize(increments)];
    last_boot += increments[rnd() % base::ArraySize(increments)];
    ct_.AddSnapshot({{MONOTONIC, last_mono}, {BOOTTIME, last_boot}});

    last_raw += increments[rnd() % base::ArraySize(increments)];
    last_boot += increments[rnd() % base::ArraySize(increments)];
    ct_.AddSnapshot({{MONOTONIC_RAW, last_raw}, {BOOTTIME, last_boot}});
  }

  for (int i = 0; i < 1000; i++) {
    int64_t val = static_cast<int64_t>(rnd()) % 10000;
    for (int j = 0; j < 5; j++) {
      ClockTracker::ClockId src;
      ClockTracker::ClockId tgt;
      if (j == 0) {
        std::tie(src, tgt) = std::make_tuple(MONOTONIC, BOOTTIME);
      } else if (j == 1) {
        std::tie(src, tgt) = std::make_tuple(MONOTONIC_RAW, BOOTTIME);
      } else if (j == 2) {
        std::tie(src, tgt) = std::make_tuple(BOOTTIME, MONOTONIC);
      } else if (j == 3) {
        std::tie(src, tgt) = std::make_tuple(BOOTTIME, MONOTONIC_RAW);
      } else if (j == 4) {
        std::tie(src, tgt) = std::make_tuple(MONOTONIC_RAW, MONOTONIC);
      } else {
        PERFETTO_FATAL("j out of bounds");
      }
      // It will still write the cache, just not lookup.
      ct_.set_cache_lookups_disabled_for_testing(true);
      auto not_cached = Convert(src, val, tgt);

      // This should 100% hit the cache.
      ct_.set_cache_lookups_disabled_for_testing(false);
      auto cached = Convert(src, val, tgt);

      ASSERT_EQ(not_cached.value(), cached.value());
    }
  }
}

// Test clock conversion with offset to the host.
TEST_F(ClockTrackerTest, ClockOffset) {
  EXPECT_FALSE(ct_.ToTraceTime(REALTIME, 0).ok());

  context_.machine_tracker =
      std::make_unique<MachineTracker>(&context_, 0x1001);

  // Client-to-host BOOTTIME offset is -10000 ns.
  ct_.SetClockOffset(BOOTTIME, -10000);

  ct_.AddSnapshot({{REALTIME, 10}, {BOOTTIME, 10010}});
  ct_.AddSnapshot({{REALTIME, 20}, {BOOTTIME, 20220}});
  ct_.AddSnapshot({{REALTIME, 30}, {BOOTTIME, 30030}});
  ct_.AddSnapshot({{MONOTONIC, 1000}, {BOOTTIME, 100000}});

  auto seq_clock_1 = ct_.SequenceToGlobalClock(1, 64);
  auto seq_clock_2 = ct_.SequenceToGlobalClock(2, 64);
  ct_.AddSnapshot({{MONOTONIC, 2000}, {seq_clock_1, 1200}});
  ct_.AddSnapshot({{seq_clock_1, 1300}, {seq_clock_2, 2000, 10, false}});

  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 0), 20000);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 1), 20001);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 9), 20009);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 10), 20010);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 11), 20011);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 19), 20019);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 20), 30220);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 21), 30221);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 29), 30229);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 30), 40030);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 40), 40040);

  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 0), 100000 - 1000 + 10000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 999), 100000 - 1 + 10000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1000), 100000 + 10000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1e6),
            static_cast<int64_t>(100000 - 1000 + 1e6 + 10000));

  // seq_clock_1 -> MONOTONIC -> BOOTTIME -> apply offset.
  EXPECT_EQ(*ct_.ToTraceTime(seq_clock_1, 1100), -100 + 1000 + 100000 + 10000);
  // seq_clock_2 -> seq_clock_1 -> MONOTONIC -> BOOTTIME -> apply offset.
  EXPECT_EQ(*ct_.ToTraceTime(seq_clock_2, 2100),
            100 * 10 + 100 + 1000 + 100000 + 10000);
}

// Test conversion of remote machine timestamps without offset. This can happen
// if timestamp conversion for remote machines is done by trace data
// post-processing.
TEST_F(ClockTrackerTest, RemoteNoClockOffset) {
  context_.machine_tracker =
      std::make_unique<MachineTracker>(&context_, 0x1001);

  ct_.AddSnapshot({{REALTIME, 10}, {BOOTTIME, 10010}});
  ct_.AddSnapshot({{REALTIME, 20}, {BOOTTIME, 20220}});
  ct_.AddSnapshot({{MONOTONIC, 1000}, {BOOTTIME, 100000}});

  auto seq_clock_1 = ct_.SequenceToGlobalClock(1, 64);
  auto seq_clock_2 = ct_.SequenceToGlobalClock(2, 64);
  ct_.AddSnapshot({{MONOTONIC, 2000}, {seq_clock_1, 1200}});
  ct_.AddSnapshot({{seq_clock_1, 1300}, {seq_clock_2, 2000, 10, false}});

  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 0), 10000);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 9), 10009);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 10), 10010);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 11), 10011);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 19), 10019);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 20), 20220);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 21), 20221);

  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 0), 100000 - 1000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 999), 100000 - 1);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1000), 100000);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1e6),
            static_cast<int64_t>(100000 - 1000 + 1e6));

  // seq_clock_1 -> MONOTONIC -> BOOTTIME.
  EXPECT_EQ(*ct_.ToTraceTime(seq_clock_1, 1100), -100 + 1000 + 100000);
  // seq_clock_2 -> seq_clock_1 -> MONOTONIC -> BOOTTIME.
  EXPECT_EQ(*ct_.ToTraceTime(seq_clock_2, 2100),
            100 * 10 + 100 + 1000 + 100000);
}

// Test clock offset of non-defualt trace time clock domain.
TEST_F(ClockTrackerTest, NonDefaultTraceTimeClock) {
  context_.machine_tracker =
      std::make_unique<MachineTracker>(&context_, 0x1001);

  ct_.SetTraceTimeClock(MONOTONIC);
  ct_.SetClockOffset(MONOTONIC, -2000);
  ct_.SetClockOffset(BOOTTIME, -10000);  // This doesn't take effect.

  ct_.AddSnapshot({{REALTIME, 10}, {BOOTTIME, 10010}});
  ct_.AddSnapshot({{MONOTONIC, 1000}, {BOOTTIME, 100000}});

  auto seq_clock_1 = ct_.SequenceToGlobalClock(1, 64);
  ct_.AddSnapshot({{MONOTONIC, 2000}, {seq_clock_1, 1200}});

  int64_t realtime_to_trace_time_delta = -10 + 10010 - 100000 + 1000 - (-2000);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 9), 9 + realtime_to_trace_time_delta);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 10), 10 + realtime_to_trace_time_delta);
  EXPECT_EQ(*ct_.ToTraceTime(REALTIME, 20), 20 + realtime_to_trace_time_delta);

  int64_t mono_to_trace_time_delta = -2000;
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 0), 0 - mono_to_trace_time_delta);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 999), 999 - mono_to_trace_time_delta);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1000), 1000 - mono_to_trace_time_delta);
  EXPECT_EQ(*ct_.ToTraceTime(MONOTONIC, 1e6),
            static_cast<int64_t>(1e6) - mono_to_trace_time_delta);

  // seq_clock_1 -> MONOTONIC.
  EXPECT_EQ(*ct_.ToTraceTime(seq_clock_1, 1100), 1100 - 1200 + 2000 - (-2000));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
