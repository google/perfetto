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
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
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
    context1.import_logs_tracker =
        TraceProcessorContextPtr<ImportLogsTracker>::MakeRoot(&context1,
                                                              TraceId{0});
    context1.stats_tracker = std::make_unique<StatsTracker>(&context1);
    context1.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context1.storage.get());
    context1.process_tracker = std::make_unique<ProcessTracker>(&context1);
    context1.sched_event_tracker =
        std::make_unique<SchedEventTracker>(&context1);
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
    context2.import_logs_tracker =
        TraceProcessorContextPtr<ImportLogsTracker>::MakeRoot(&context2,
                                                              TraceId{1});
    context2.stats_tracker = std::make_unique<StatsTracker>(&context2);
    context2.global_args_tracker = context1.global_args_tracker.Fork();
    context2.process_tracker = context1.process_tracker.Fork();
    context2.sched_event_tracker =
        std::make_unique<SchedEventTracker>(&context2);
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

// Two distinct machine tracks of the SAME type (e.g. CPU 0 vs CPU 1 frequency)
// can be owned by DIFFERENT traces independently without conflict.
TEST_F(MachineDataClaimTrackerTest,
       DistinctTracksOfSameTypeClaimedIndependently) {
  TrackId cpu0_track = context1.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(0));
  TrackId cpu1_track = context2.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(1));

  // Trace 1 claims CPU 0 track.
  EXPECT_TRUE(
      context1.event_tracker->PushCounter(100, 1000.0, cpu0_track).has_value());

  // Trace 2 claims CPU 1 track (same type, different track). Both kept, 0
  // conflicts.
  EXPECT_TRUE(
      context2.event_tracker->PushCounter(101, 1500.0, cpu1_track).has_value());

  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_counter_claim_conflict),
            0);
  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
                stats::machine_counter_claim_conflict),
            0);

  // Trace 2 pushing to the SAME track (CPU 0) is dropped and counted.
  EXPECT_FALSE(
      context2.event_tracker->PushCounter(102, 2000.0, cpu0_track).has_value());

  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
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
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(&context1,
                                                 SchedEventKind::kOther, 100));

  // Trace 2 calls KeepSched with kSwitch; successfully claims sched for
  // trace 2.
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(&context2,
                                                 SchedEventKind::kSwitch, 200));

  // Trace 2 can continue.
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(&context2,
                                                 SchedEventKind::kOther, 300));

  // Trace 1 is now rejected on kOther because trace 2 owns sched.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(&context1,
                                                  SchedEventKind::kOther, 400));
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
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(&context1,
                                                 SchedEventKind::kSwitch, 100));

  // Trace 2 claims cpu_frequency.
  EXPECT_TRUE(
      context2.event_tracker->PushCounter(100, 1500.0, freq_track).has_value());

  // Trace 2 fails sched.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(
      &context2, SchedEventKind::kSwitch, 200));

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

// task_newtask duplicate tracking remembers applied events and reuses utid.
TEST_F(MachineDataClaimTrackerTest, TaskNewTaskDeduplication) {
  auto* tracker = context1.machine_data_claim_tracker.get();
  EXPECT_FALSE(tracker->FindTaskNewTask(1000, 42).has_value());
  tracker->RecordTaskNewTask(1000, 42, 7);
  // Lookup at same ts finds it.
  EXPECT_EQ(tracker->FindTaskNewTask(1000, 42),
            std::make_optional<UniqueTid>(7));
  // Accessible from context2 (shared per-machine state) at same ts.
  auto* tracker2 = context2.machine_data_claim_tracker.get();
  EXPECT_EQ(tracker2->FindTaskNewTask(1000, 42),
            std::make_optional<UniqueTid>(7));

  // Different pid at same ts does not match.
  EXPECT_FALSE(tracker->FindTaskNewTask(1000, 43).has_value());

  // Advancing to a later ts clears previous entries.
  tracker->RecordTaskNewTask(1001, 43, 8);
  EXPECT_EQ(tracker->FindTaskNewTask(1001, 43),
            std::make_optional<UniqueTid>(8));
  // Old entry from ts 1000 is gone and not found even when queried at ts 1001.
  EXPECT_FALSE(tracker->FindTaskNewTask(1001, 42).has_value());
}

// First drop per (trace, kind) is recorded in import logs table.
TEST_F(MachineDataClaimTrackerTest, ImportLogsRecordedOnFirstDrop) {
  TrackId freq_track = context1.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(0));

  // Trace 1 claims cpu_frequency.
  EXPECT_TRUE(
      context1.event_tracker->PushCounter(100, 1000.0, freq_track).has_value());

  // Trace 2's first drop logs to import_logs.
  EXPECT_FALSE(
      context2.event_tracker->PushCounter(101, 2000.0, freq_track).has_value());
  const auto& logs = context1.storage->trace_import_logs_table();
  EXPECT_EQ(logs.row_count(), 1u);
  EXPECT_TRUE(logs[0].arg_set_id().has_value());

  // Subsequent drops from Trace 2 do not add another import log.
  EXPECT_FALSE(
      context2.event_tracker->PushCounter(102, 3000.0, freq_track).has_value());
  EXPECT_EQ(logs.row_count(), 1u);

  // Trace 1 claims sched.
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(&context1,
                                                 SchedEventKind::kSwitch, 200));

  // Trace 2's first sched drop logs to import_logs.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(
      &context2, SchedEventKind::kSwitch, 201));
  EXPECT_EQ(logs.row_count(), 2u);
  EXPECT_TRUE(logs[1].arg_set_id().has_value());

  // Subsequent sched drop from Trace 2 does not add another log.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(
      &context2, SchedEventKind::kSwitch, 202));
  EXPECT_EQ(logs.row_count(), 2u);

  // Verify owner_trace_id arg in import logs equals the owning trace (Trace 1,
  // id 0).
  StringId owner_key = context1.storage->InternString("owner_trace_id");
  std::optional<int64_t> counter_owner;
  std::optional<int64_t> sched_owner;
  const auto& args = context1.storage->arg_table();
  for (auto it = args.IterateRows(); it; ++it) {
    if (it.arg_set_id() == *logs[0].arg_set_id() && it.key() == owner_key) {
      counter_owner = it.int_value();
    }
    if (it.arg_set_id() == *logs[1].arg_set_id() && it.key() == owner_key) {
      sched_owner = it.int_value();
    }
  }
  EXPECT_EQ(counter_owner, std::make_optional<int64_t>(0));
  EXPECT_EQ(sched_owner, std::make_optional<int64_t>(0));
}

TEST_F(MachineDataClaimTrackerTest, FirstClaimClosesOtherTracesOpenStates) {
  // Trace 2 has an early waking event before any trace claims sched.
  ThreadStateTracker* t2_tst = ThreadStateTracker::GetOrCreate(&context2);
  UniqueTid utid = 1;
  t2_tst->PushWakingEvent(1000, utid, /*waker_utid=*/0);

  // Verify Trace 2 has an open state (dur == -1).
  const auto& table = context2.storage->thread_state_table();
  ASSERT_EQ(table.row_count(), 1u);
  EXPECT_EQ(table[0].dur(), -1);
  EXPECT_EQ(table[0].ts(), 1000);

  // Trace 1 now claims sched at ts 2000.
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(
      &context1, SchedEventKind::kSwitch, 2000));

  // Verify Trace 2's open state was closed at ts 2000 (dur = 1000).
  EXPECT_EQ(table[0].dur(), 1000);
}

// Leases + drop inside lease + handover after lease.
TEST_F(MachineDataClaimTrackerTest, SchedLeasesDropAndHandover) {
  MachineDataClaimTracker::NoteSchedData(&context1, 1000);
  MachineDataClaimTracker::NoteSchedData(&context1, 2000);
  MachineDataClaimTracker::NoteSchedData(&context2, 1500);
  MachineDataClaimTracker::NoteSchedData(&context2, 3000);
  MachineDataClaimTracker::NotifyTokenizationDone(&context1);

  // Traces register sched closers so they can hand over.
  context1.machine_data_claim_tracker->RegisterSchedCloser(TraceId{0},
                                                           [](int64_t) {});
  context1.machine_data_claim_tracker->RegisterSchedCloser(TraceId{1},
                                                           [](int64_t) {});

  // Trace 1 claims sched at ts 1000; lease ends at 2000.
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(
      &context1, SchedEventKind::kSwitch, 1000));
  EXPECT_EQ(context1.machine_data_claim_tracker->sched_owner_for_testing(),
            std::make_optional(TraceId{0}));
  EXPECT_EQ(
      context1.machine_data_claim_tracker->sched_owner_lease_end_for_testing(),
      2000);

  // Trace 2 switch at 1500 is inside Trace 1's lease -> dropped.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(
      &context2, SchedEventKind::kSwitch, 1500));
  EXPECT_EQ(context2.global_stats_tracker->GetStats(
                context2.machine_id(), context2.trace_id(),
                stats::machine_sched_claim_conflict),
            1);

  // Trace 2 switch at 2500 is after Trace 1's lease end -> handover!
  EXPECT_TRUE(MachineDataClaimTracker::KeepSched(
      &context2, SchedEventKind::kSwitch, 2500));
  EXPECT_EQ(context1.machine_data_claim_tracker->sched_owner_for_testing(),
            std::make_optional(TraceId{1}));
  EXPECT_EQ(
      context1.machine_data_claim_tracker->sched_owner_lease_end_for_testing(),
      3000);

  // Trace 1 switch at 2600 is inside Trace 2's lease -> dropped.
  EXPECT_FALSE(MachineDataClaimTracker::KeepSched(
      &context1, SchedEventKind::kSwitch, 2600));
  EXPECT_EQ(context1.global_stats_tracker->GetStats(
                context1.machine_id(), context1.trace_id(),
                stats::machine_sched_claim_conflict),
            1);
}

// Counter track handover: write after owner lease end becomes new owner.
TEST_F(MachineDataClaimTrackerTest, CounterHandoverAfterLease) {
  TrackId freq_track = context1.track_tracker->InternTrack(
      tracks::kCpuFrequencyBlueprint, tracks::Dimensions(0));

  MachineDataClaimTracker::NoteData(
      &context1, MachineDataClaimTracker::SourceKind::kFtrace, 1000);
  MachineDataClaimTracker::NoteData(
      &context1, MachineDataClaimTracker::SourceKind::kFtrace, 2000);

  MachineDataClaimTracker::NoteData(
      &context2, MachineDataClaimTracker::SourceKind::kFtrace, 1500);
  MachineDataClaimTracker::NoteData(
      &context2, MachineDataClaimTracker::SourceKind::kFtrace, 3000);

  MachineDataClaimTracker::NotifyTokenizationDone(&context1);

  // Trace 1 claims freq_track at 1000; lease ends at 2000.
  EXPECT_TRUE(
      context1.event_tracker->PushCounter(1000, 100.0, freq_track).has_value());

  // Trace 2 pushes at 1500; dropped inside Trace 1 lease.
  EXPECT_FALSE(
      context2.event_tracker->PushCounter(1500, 200.0, freq_track).has_value());

  // At ts 2500, Trace 2 writes to freq_track after Trace 1's lease end (2000)
  // -> handover!
  EXPECT_TRUE(
      context2.event_tracker->PushCounter(2500, 300.0, freq_track).has_value());

  EXPECT_EQ(context1.machine_data_claim_tracker->counter_owner_for_testing(
                freq_track),
            std::make_optional(TraceId{1}));

  // Verify counter table on freq_track has exactly two rows: ts 1000 (100.0)
  // and ts 2500 (300.0).
  const auto& counter_table = context1.storage->counter_table();
  size_t freq_rows = 0;
  for (auto it = counter_table.IterateRows(); it; ++it) {
    if (it.track_id() == freq_track) {
      ++freq_rows;
      EXPECT_NE(it.value(), 200.0);
    }
  }
  EXPECT_EQ(freq_rows, 2u);
}

}  // namespace
}  // namespace perfetto::trace_processor
