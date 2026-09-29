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

#include "src/trace_processor/importers/common/machine_data_claim_tracker.h"

#include <cstdint>
#include <memory>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using SchedEventKind = MachineDataClaimTracker::SchedEventKind;

class MachineDataClaimTrackerTest : public ::testing::Test {
 public:
  MachineDataClaimTrackerTest() {
    context1.storage = std::make_unique<TraceStorage>();
    context1.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context1.storage.get());
    context1.machine_tracker =
        std::make_unique<MachineTracker>(&context1, kDefaultMachineId);
    context1.cpu_tracker = std::make_unique<CpuTracker>(&context1);
    context1.machine_data_claim_tracker =
        std::make_unique<MachineDataClaimTracker>(&context1);
    context1.machine_track_tracker =
        TraceProcessorContextPtr<TrackTracker>::MakeRoot(&context1);
    context1.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context1.stats_tracker = std::make_unique<StatsTracker>(&context1);
    context1.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context1.storage.get());
    context1.process_tracker = std::make_unique<ProcessTracker>(&context1);
    context1.track_tracker = std::make_unique<TrackTracker>(&context1);
    context1.event_tracker = std::make_unique<EventTracker>(&context1);

    context2.storage = context1.storage.Fork();
    context2.global_stats_tracker = context1.global_stats_tracker.Fork();
    context2.machine_tracker = context1.machine_tracker.Fork();
    context2.cpu_tracker = context1.cpu_tracker.Fork();
    context2.machine_data_claim_tracker =
        context1.machine_data_claim_tracker.Fork();
    context2.machine_track_tracker = context1.machine_track_tracker.Fork();
    context2.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{1}});
    context2.stats_tracker = std::make_unique<StatsTracker>(&context2);
    context2.global_args_tracker = context1.global_args_tracker.Fork();
    context2.process_tracker = context1.process_tracker.Fork();
    context2.track_tracker = std::make_unique<TrackTracker>(&context2);
    context2.event_tracker = std::make_unique<EventTracker>(&context2);
  }

 protected:
  TraceProcessorContext context1;
  TraceProcessorContext context2;
};

// Two distinct machine track types (e.g. cpu_frequency vs cpu_idle) can be
// owned by DIFFERENT traces independently.
TEST_F(MachineDataClaimTrackerTest,
       IndependentTrackTypesOwnedByDifferentTraces) {
  TrackId freq_track = context1.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(0));
  TrackId idle_track = context2.track_tracker->InternTrack(
      tracks::kCpuIdleBlueprint, tracks::Dimensions(0));

  // Trace 1 claims cpu_frequency.
  EXPECT_TRUE(
      context1.event_tracker->PushCounter(100, 1000.0, freq_track).has_value());

  // Trace 2 claims cpu_idle.
  EXPECT_TRUE(
      context2.event_tracker->PushCounter(100, 1.0, idle_track).has_value());

  // Trace 2 is rejected on cpu_frequency.
  EXPECT_FALSE(
      context2.event_tracker->PushCounter(101, 2000.0, freq_track).has_value());

  // Trace 1 is rejected on cpu_idle.
  EXPECT_FALSE(
      context1.event_tracker->PushCounter(101, 2.0, idle_track).has_value());

  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
                stats::machine_counter_claim_conflict),
            1);
  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_counter_claim_conflict),
            1);
}

// An unregistered (non-machine) track is never dropped.
TEST_F(MachineDataClaimTrackerTest, NonMachineTrackNeverDropped) {
  TrackId non_machine_track = context1.track_tracker->InternTrack(
      tracks::kClockStateBlueprint, tracks::Dimensions("clk"));

  EXPECT_TRUE(context1.event_tracker->PushCounter(100, 10.0, non_machine_track)
                  .has_value());
  EXPECT_TRUE(context2.event_tracker->PushCounter(101, 20.0, non_machine_track)
                  .has_value());

  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_counter_claim_conflict),
            0);
  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
                stats::machine_counter_claim_conflict),
            0);
}

// KeepSched with kOther does not claim scheduling ownership.
TEST_F(MachineDataClaimTrackerTest, KeepSchedOtherDoesNotClaim) {
  // Trace 1 calls KeepSched with kOther when unclaimed; returns true but does
  // not claim ownership.
  EXPECT_TRUE(
      MachineDataClaimTracker::KeepSched(&context1, SchedEventKind::kOther));

  // Trace 2 calls KeepSched with kSwitch; successfully claims sched for
  // trace 2.
  EXPECT_TRUE(
      MachineDataClaimTracker::KeepSched(&context2, SchedEventKind::kSwitch));

  // Trace 2 can continue.
  EXPECT_TRUE(
      MachineDataClaimTracker::KeepSched(&context2, SchedEventKind::kOther));

  // Trace 1 is now rejected on kOther because trace 2 owns sched.
  EXPECT_FALSE(
      MachineDataClaimTracker::KeepSched(&context1, SchedEventKind::kOther));
  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_sched_claim_conflict),
            1);
}

// Sched and counters are independent (trace A can own sched, trace B can own
// cpu_frequency).
TEST_F(MachineDataClaimTrackerTest, SchedAndCountersIndependent) {
  TrackId freq_track = context2.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(0));

  // Trace 1 claims sched.
  EXPECT_TRUE(
      MachineDataClaimTracker::KeepSched(&context1, SchedEventKind::kSwitch));

  // Trace 2 claims cpu_frequency.
  EXPECT_TRUE(
      context2.event_tracker->PushCounter(100, 1500.0, freq_track).has_value());

  // Trace 2 fails sched.
  EXPECT_FALSE(
      MachineDataClaimTracker::KeepSched(&context2, SchedEventKind::kSwitch));

  // Trace 1 fails cpu_frequency.
  EXPECT_FALSE(
      context1.event_tracker->PushCounter(101, 2000.0, freq_track).has_value());

  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
                stats::machine_sched_claim_conflict),
            1);
  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_counter_claim_conflict),
            1);
}

}  // namespace
}  // namespace perfetto::trace_processor
