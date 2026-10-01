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
#include <vector>

#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {

namespace {

using Kind = MachineDataClaimTracker::Kind;
using Window = MachineDataClaimTracker::WindowForTesting;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class MachineDataClaimTrackerTest : public ::testing::Test {
 protected:
  MachineDataClaimTrackerTest() {
    machine_ctx_.config.drop_duplicate_machine_data =
        Config::DropDuplicateMachineData::kOn;
    machine_ctx_.storage = std::make_unique<TraceStorage>();
    machine_ctx_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(machine_ctx_.storage.get());
    machine_ctx_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(machine_ctx_.storage.get());
    machine_ctx_.machine_tracker =
        std::make_unique<MachineTracker>(&machine_ctx_, 0);
    machine_ctx_.machine_data_claim_tracker =
        std::make_unique<MachineDataClaimTracker>(&machine_ctx_);
    InitTraceContext(&trace1_, TraceId(1));
    InitTraceContext(&trace2_, TraceId(2));
    InitTraceContext(&trace3_, TraceId(3));
  }

  void InitTraceContext(TraceProcessorContext* ctx, TraceId trace_id) {
    ctx->config = machine_ctx_.config;
    ctx->storage = machine_ctx_.storage.Fork();
    ctx->global_args_tracker = machine_ctx_.global_args_tracker.Fork();
    ctx->global_stats_tracker = machine_ctx_.global_stats_tracker.Fork();
    ctx->machine_tracker = machine_ctx_.machine_tracker.Fork();
    ctx->machine_data_claim_tracker =
        machine_ctx_.machine_data_claim_tracker.Fork();
    ctx->trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{trace_id});
    ctx->stats_tracker = std::make_unique<StatsTracker>(ctx);
    ctx->import_logs_tracker =
        std::make_unique<ImportLogsTracker>(ctx, trace_id);
  }

  bool Import(TraceProcessorContext* ctx,
              int64_t ts,
              Kind kind = Kind::kSched) {
    return tracker()->ShouldImport(ctx, kind, ts);
  }

  std::vector<Window> Windows(Kind kind = Kind::kSched) {
    return tracker()->GetWindowsForTesting(kind);
  }

  MachineDataClaimTracker* tracker() {
    return machine_ctx_.machine_data_claim_tracker.get();
  }

  int64_t Drops(TraceProcessorContext* ctx, Kind kind = Kind::kSched) {
    return ctx->stats_tracker->GetStats(
        MachineDataClaimTracker::StatForKind(kind));
  }

  uint32_t ImportLogCount() {
    return machine_ctx_.storage->trace_import_logs_table().row_count();
  }

  TraceProcessorContext machine_ctx_;
  TraceProcessorContext trace1_;
  TraceProcessorContext trace2_;
  TraceProcessorContext trace3_;
};

TEST_F(MachineDataClaimTrackerTest, OffByDefault) {
  TraceProcessorContext off_ctx;
  off_ctx.storage = machine_ctx_.storage.Fork();
  off_ctx.global_args_tracker = machine_ctx_.global_args_tracker.Fork();
  off_ctx.global_stats_tracker = machine_ctx_.global_stats_tracker.Fork();
  off_ctx.machine_tracker = machine_ctx_.machine_tracker.Fork();
  off_ctx.machine_data_claim_tracker =
      machine_ctx_.machine_data_claim_tracker.Fork();
  off_ctx.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId(10)});
  off_ctx.stats_tracker = std::make_unique<StatsTracker>(&off_ctx);
  off_ctx.import_logs_tracker =
      std::make_unique<ImportLogsTracker>(&off_ctx, TraceId(10));

  // Default is OFF: ShouldImport returns true and does not create windows.
  EXPECT_FALSE(MachineDataClaimTracker::IsEnabled(&off_ctx));
  EXPECT_TRUE(off_ctx.machine_data_claim_tracker->ShouldImport(
      &off_ctx, Kind::kSched, 100));
  EXPECT_TRUE(off_ctx.machine_data_claim_tracker->ShouldImport(
      &off_ctx, Kind::kSched, 100));
}

TEST_F(MachineDataClaimTrackerTest, SingleTraceImportsEverything) {
  EXPECT_THAT(Windows(), IsEmpty());

  EXPECT_TRUE(Import(&trace1_, 100));
  EXPECT_TRUE(Import(&trace1_, 50));
  EXPECT_TRUE(Import(&trace1_, 200));
  EXPECT_TRUE(Import(&trace1_, 150));

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 50, 200}));
  EXPECT_EQ(Drops(&trace1_), 0);
  EXPECT_EQ(ImportLogCount(), 0u);
}

TEST_F(MachineDataClaimTrackerTest, NonOverlappingTracesAreBothImported) {
  ASSERT_TRUE(Import(&trace1_, 100));
  ASSERT_TRUE(Import(&trace1_, 200));

  // After the first trace.
  EXPECT_TRUE(Import(&trace2_, 300));
  EXPECT_TRUE(Import(&trace2_, 201));
  EXPECT_TRUE(Import(&trace2_, 400));

  // Before both.
  EXPECT_TRUE(Import(&trace3_, 50));
  EXPECT_TRUE(Import(&trace3_, 99));

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 100, 200},
                                     Window{TraceId(2), 201, 400},
                                     Window{TraceId(3), 50, 99}));
  EXPECT_EQ(Drops(&trace1_) + Drops(&trace2_) + Drops(&trace3_), 0);
  EXPECT_EQ(ImportLogCount(), 0u);
}

TEST_F(MachineDataClaimTrackerTest, OverlappingDataOfLaterTraceIsDropped) {
  ASSERT_TRUE(Import(&trace1_, 100));
  ASSERT_TRUE(Import(&trace1_, 200));

  // Inside the first trace's window.
  EXPECT_FALSE(Import(&trace2_, 100));
  EXPECT_FALSE(Import(&trace2_, 150));
  EXPECT_FALSE(Import(&trace2_, 200));
  // After it.
  EXPECT_TRUE(Import(&trace2_, 250));
  EXPECT_FALSE(Import(&trace2_, 180));
  EXPECT_TRUE(Import(&trace2_, 300));

  // Every drop is counted against the dropping trace...
  EXPECT_EQ(Drops(&trace2_), 4);
  EXPECT_EQ(Drops(&trace1_), 0);
  // ...but only logged once.
  EXPECT_EQ(ImportLogCount(), 1u);

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 100, 200},
                                     Window{TraceId(2), 250, 300}));
}

TEST_F(MachineDataClaimTrackerTest, TraceContributesSingleWindow) {
  ASSERT_TRUE(Import(&trace1_, 100));
  ASSERT_TRUE(Import(&trace1_, 200));

  // The second trace wraps the first: it only keeps the side it started on.
  EXPECT_TRUE(Import(&trace2_, 300));
  EXPECT_FALSE(Import(&trace2_, 50));
  EXPECT_TRUE(Import(&trace2_, 201));

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 100, 200},
                                     Window{TraceId(2), 201, 300}));
  EXPECT_EQ(Drops(&trace2_), 1);
}

TEST_F(MachineDataClaimTrackerTest, EarlierWindowsAreFrozen) {
  ASSERT_TRUE(Import(&trace1_, 100));
  ASSERT_TRUE(Import(&trace1_, 200));
  ASSERT_TRUE(Import(&trace2_, 300));

  // Once another trace claimed a window, the first trace's window can't grow,
  // even into time nobody claimed.
  EXPECT_FALSE(Import(&trace1_, 250));
  EXPECT_FALSE(Import(&trace1_, 50));
  EXPECT_TRUE(Import(&trace1_, 150));

  // The latest trace's window still grows.
  EXPECT_TRUE(Import(&trace2_, 1000));

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 100, 200},
                                     Window{TraceId(2), 300, 1000}));
  EXPECT_EQ(Drops(&trace1_), 2);
}

TEST_F(MachineDataClaimTrackerTest, TraceClaimsGapBetweenWindows) {
  ASSERT_TRUE(Import(&trace1_, 100));
  ASSERT_TRUE(Import(&trace1_, 200));
  ASSERT_TRUE(Import(&trace2_, 400));
  ASSERT_TRUE(Import(&trace2_, 500));

  EXPECT_TRUE(Import(&trace3_, 300));
  EXPECT_TRUE(Import(&trace3_, 201));
  EXPECT_TRUE(Import(&trace3_, 399));
  EXPECT_FALSE(Import(&trace3_, 200));
  EXPECT_FALSE(Import(&trace3_, 400));
  EXPECT_FALSE(Import(&trace3_, 600));

  EXPECT_THAT(Windows(), ElementsAre(Window{TraceId(1), 100, 200},
                                     Window{TraceId(2), 400, 500},
                                     Window{TraceId(3), 201, 399}));
  EXPECT_EQ(Drops(&trace3_), 3);
  EXPECT_EQ(ImportLogCount(), 1u);
}

TEST_F(MachineDataClaimTrackerTest, KindsAreIndependent) {
  ASSERT_TRUE(Import(&trace1_, 100, Kind::kSched));
  ASSERT_TRUE(Import(&trace1_, 200, Kind::kSched));

  // Different kinds of data recorded at the same time by different traces
  // are all imported.
  EXPECT_TRUE(Import(&trace2_, 150, Kind::kSysStats));
  EXPECT_TRUE(Import(&trace3_, 150, Kind::kAndroidPower));

  // Each kind is arbitrated separately.
  EXPECT_FALSE(Import(&trace2_, 150, Kind::kSched));
  EXPECT_FALSE(Import(&trace1_, 150, Kind::kSysStats));

  EXPECT_THAT(Windows(Kind::kSched),
              ElementsAre(Window{TraceId(1), 100, 200}));
  EXPECT_THAT(Windows(Kind::kSysStats),
              ElementsAre(Window{TraceId(2), 150, 150}));
  EXPECT_THAT(Windows(Kind::kAndroidPower),
              ElementsAre(Window{TraceId(3), 150, 150}));
  EXPECT_THAT(Windows(Kind::kCpuPerUid), IsEmpty());

  // One log per (trace, kind).
  EXPECT_EQ(ImportLogCount(), 2u);
}

TEST_F(MachineDataClaimTrackerTest, ExclusiveKindIsOwnedByFirstTrace) {
  ASSERT_TRUE(MachineDataClaimTracker::IsExclusive(Kind::kCpuPerUid));
  ASSERT_TRUE(Import(&trace1_, 100, Kind::kCpuPerUid));
  ASSERT_TRUE(Import(&trace1_, 200, Kind::kCpuPerUid));

  // Even when it doesn't overlap.
  EXPECT_FALSE(Import(&trace2_, 1000, Kind::kCpuPerUid));
  EXPECT_FALSE(Import(&trace2_, 10, Kind::kCpuPerUid));

  // The owner's window keeps growing.
  EXPECT_TRUE(Import(&trace1_, 2000, Kind::kCpuPerUid));

  EXPECT_THAT(Windows(Kind::kCpuPerUid),
              ElementsAre(Window{TraceId(1), 100, 2000}));
  EXPECT_EQ(Drops(&trace2_, Kind::kCpuPerUid), 2);
}

TEST_F(MachineDataClaimTrackerTest, KindForTracePacketField) {
  using protos::pbzero::TracePacket;
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kSysStatsFieldNumber),
            Kind::kSysStats);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kGenericKernelTaskStateEventFieldNumber),
            Kind::kSched);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kGenericKernelCpuFreqEventFieldNumber),
            Kind::kCounter);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kBatteryFieldNumber),
            Kind::kAndroidPower);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kCpuPerUidDataFieldNumber),
            Kind::kCpuPerUid);
  // ftrace is arbitrated per event, not per packet.
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kFtraceEventsFieldNumber),
            std::nullopt);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kTrackEventFieldNumber),
            std::nullopt);
  EXPECT_EQ(MachineDataClaimTracker::KindForTracePacketField(
                TracePacket::kProcessTreeFieldNumber),
            std::nullopt);
}

TEST_F(MachineDataClaimTrackerTest, KindToString) {
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kSched), "sched");
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kFtrace), "ftrace");
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kSysStats),
               "sys_stats");
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kCpuPerUid),
               "cpu_per_uid");
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kAndroidPower),
               "android_power");
  EXPECT_STREQ(MachineDataClaimTracker::KindToString(Kind::kCounter), "counter");
}

}  // namespace
}  // namespace perfetto::trace_processor
