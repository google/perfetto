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

#include "src/trace_processor/importers/syscalls/syscall_tracker.h"

#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

// These tests drive a real SliceTracker (SliceTracker's slice methods are
// templated on the args callback and no longer virtual, so they cannot be
// mocked) and assert on the slices that end up in storage.
class SyscallTrackerTest : public ::testing::Test {
 public:
  SyscallTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context.storage.get());
    context.machine_tracker.reset(
        new MachineTracker(&context, kDefaultMachineId));
    context.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context.stats_tracker = std::make_unique<StatsTracker>(&context);
    context.global_args_tracker.reset(
        new GlobalArgsTracker(context.storage.get()));
    context.args_translation_table =
        std::make_unique<ArgsTranslationTable>(context.storage.get());
    context.slice_translation_table =
        std::make_unique<SliceTranslationTable>(context.storage.get());
    track_tracker = new TrackTracker(&context);
    context.track_tracker.reset(track_tracker);
    context.slice_tracker.reset(new SliceTracker(&context));
  }

  const tables::SliceTable& slices() const {
    return context.storage->slice_table();
  }

  std::string SliceName(uint32_t row) const {
    return context.storage
        ->GetString(slices()[row].name().value_or(kNullStringId))
        .ToStdString();
  }

 protected:
  TraceProcessorContext context;
  TrackTracker* track_tracker;
};

TEST_F(SyscallTrackerTest, ReportUnknownSyscalls) {
  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(&context);
  syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 57 /*sys_read*/);
  syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 57 /*sys_read*/);

  ASSERT_EQ(slices().row_count(), 1u);
  EXPECT_EQ(slices()[0].ts(), 100);
  EXPECT_EQ(slices()[0].dur(), 10);
  EXPECT_EQ(SliceName(0), "sys_57");
}

TEST_F(SyscallTrackerTest, ReportSysreturn) {
  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(&context);
  syscall_tracker->SetArchitecture(Architecture::kArm64);
  syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 139);

  // sys_rt_sigreturn does not return, so it is emitted as an instant (Scoped)
  // slice with zero duration from the Enter event alone.
  ASSERT_EQ(slices().row_count(), 1u);
  EXPECT_EQ(slices()[0].ts(), 100);
  EXPECT_EQ(slices()[0].dur(), 0);
  EXPECT_EQ(SliceName(0), "sys_rt_sigreturn");
}

TEST_F(SyscallTrackerTest, Arm64) {
  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(&context);
  syscall_tracker->SetArchitecture(Architecture::kArm64);
  syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 63 /*sys_read*/);
  syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 63 /*sys_read*/);

  ASSERT_EQ(slices().row_count(), 1u);
  EXPECT_EQ(slices()[0].ts(), 100);
  EXPECT_EQ(slices()[0].dur(), 10);
  EXPECT_EQ(SliceName(0), "sys_read");
}

TEST_F(SyscallTrackerTest, x8664) {
  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(&context);
  syscall_tracker->SetArchitecture(Architecture::kX86_64);
  syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 0 /*sys_read*/);
  syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 0 /*sys_read*/);

  ASSERT_EQ(slices().row_count(), 1u);
  EXPECT_EQ(slices()[0].ts(), 100);
  EXPECT_EQ(slices()[0].dur(), 10);
  EXPECT_EQ(SliceName(0), "sys_read");
}

TEST_F(SyscallTrackerTest, SyscallNumberTooLarge) {
  SyscallTracker* syscall_tracker = SyscallTracker::GetOrCreate(&context);
  syscall_tracker->SetArchitecture(Architecture::kArm64);
  syscall_tracker->Enter(100 /*ts*/, 42 /*utid*/, 9999);
  syscall_tracker->Exit(110 /*ts*/, 42 /*utid*/, 9999);

  EXPECT_EQ(slices().row_count(), 0u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
