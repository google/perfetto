/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/importers/common/process_tracker.h"

#include <memory>
#include <optional>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;

class ProcessTrackerTest : public ::testing::Test {
 public:
  ProcessTrackerTest() {
    context.storage = std::make_unique<TraceStorage>();
    context.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context.storage.get());
    context.machine_tracker =
        std::make_unique<MachineTracker>(&context, kDefaultMachineId);
    context.process_tracker = std::make_unique<ProcessTracker>(&context);
    context.event_tracker = std::make_unique<EventTracker>(&context);
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(ProcessTrackerTest, GetOrCreateProcess) {
  auto upid = context.process_tracker->GetOrCreateProcess(123);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(123), upid);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(123).has_value());
}

TEST_F(ProcessTrackerTest, GetOrCreateProcessWithoutMainThread) {
  auto upid = context.process_tracker->GetOrCreateProcessWithoutMainThread(123);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(123), upid);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(123).has_value());
}

TEST_F(ProcessTrackerTest, StartNewProcess) {
  auto upid = context.process_tracker->StartNewProcess(
      1000, 0u, 123, kNullStringId, ThreadNamePriority::kFtrace);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(123), upid);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(123).has_value());
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
}

TEST_F(ProcessTrackerTest, StartNewProcessWithoutMainThread) {
  auto upid = context.process_tracker->StartNewProcessWithoutMainThread(
      1000, 0u, 123, kNullStringId, ThreadNamePriority::kGenericKernelTask);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(123), upid);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(123).has_value());
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
}

TEST_F(ProcessTrackerTest, StartNewProcessWithoutMainThread_withUpdateThread) {
  auto upid = context.process_tracker->StartNewProcessWithoutMainThread(
      1000, 0u, 123, kNullStringId, ThreadNamePriority::kGenericKernelTask);

  context.process_tracker->UpdateThread(12345, 123);

  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(123), upid);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(123).has_value());
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(12345).has_value());
}

TEST_F(ProcessTrackerTest, UpdateProcessWithParent) {
  UniquePid cur_upid;
  std::optional<UniquePid> cur_pupid;
  UniquePid pupid1 = context.process_tracker->GetOrCreateProcess(123);
  UniquePid pupid2 = context.process_tracker->GetOrCreateProcess(234);
  UniquePid upid = context.process_tracker->GetOrCreateProcess(345);

  cur_upid =
      context.process_tracker->UpdateProcessWithParent(upid, pupid1, true);
  cur_pupid = context.storage->process_table()[cur_upid].parent_upid();

  ASSERT_EQ(upid, cur_upid);
  ASSERT_EQ(pupid1, *cur_pupid);

  // Must create new process
  cur_upid =
      context.process_tracker->UpdateProcessWithParent(upid, pupid2, true);
  cur_pupid = context.storage->process_table()[cur_upid].parent_upid();

  ASSERT_NE(upid, cur_upid);
  ASSERT_EQ(pupid2, *cur_pupid);
}

TEST_F(ProcessTrackerTest, SetProcessMetadata) {
  UniquePid upid = context.process_tracker->GetOrCreateProcess(123);

  context.process_tracker->SetProcessMetadata(upid, "test", "cmdline blah");

  auto opt_upid = context.process_tracker->UpidForPidForTesting(123);
  auto name = context.storage->process_table()[upid].name();
  auto cmdline = *context.storage->process_table()[upid].cmdline();

  ASSERT_EQ(opt_upid.value_or(-1), upid);
  ASSERT_EQ(context.storage->GetString(*name), "test");
  ASSERT_EQ(context.storage->GetString(cmdline), "cmdline blah");
}

TEST_F(ProcessTrackerTest, UpdateThreadCreate) {
  context.process_tracker->UpdateThread(12, 2);

  // We expect 3 threads: Invalid thread, main thread for pid, tid 12.
  ASSERT_EQ(context.storage->thread_table().row_count(), 3u);

  auto tid_it = context.process_tracker->UtidsForTidForTesting(12);
  ASSERT_NE(tid_it.first, tid_it.second);
  ASSERT_EQ(context.storage->thread_table()[1].upid().value(), 1u);
  auto opt_upid = context.process_tracker->UpidForPidForTesting(2);
  ASSERT_TRUE(opt_upid.has_value());
  ASSERT_EQ(context.storage->process_table().row_count(), 2u);
}

TEST_F(ProcessTrackerTest, UpdateThread_withStartNewProcessWithoutMainThread) {
  context.process_tracker->UpdateThread(12, 2);

  auto opt_orig_upid = context.process_tracker->UpidForPidForTesting(2);
  ASSERT_TRUE(opt_orig_upid.has_value());
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(2), *opt_orig_upid);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(2).has_value());

  // Should override the previous created process
  auto upid = context.process_tracker->StartNewProcessWithoutMainThread(
      1000, 0u, 2, kNullStringId, ThreadNamePriority::kGenericKernelTask);

  ASSERT_NE(*opt_orig_upid, upid);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(2), upid);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(2).has_value());
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
}

TEST_F(ProcessTrackerTest,
       UpdateThread_withGetOrCreateProcessWithoutMainThread) {
  context.process_tracker->UpdateThread(12, 2);

  auto opt_orig_upid = context.process_tracker->UpidForPidForTesting(2);
  ASSERT_TRUE(opt_orig_upid.has_value());
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(2), *opt_orig_upid);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(2).has_value());

  auto upid = context.process_tracker->GetOrCreateProcessWithoutMainThread(2);

  ASSERT_EQ(*opt_orig_upid, upid);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(2), *opt_orig_upid);
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(2).has_value());
}

TEST_F(ProcessTrackerTest, PidReuseWithoutStartAndEndThread) {
  UniquePid p1 = context.process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/1, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniqueTid t1 = context.process_tracker->UpdateThread(/*tid=*/2, /*pid=*/1);

  UniquePid p2 = context.process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/1, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniqueTid t2 = context.process_tracker->UpdateThread(/*tid=*/2, /*pid=*/1);

  ASSERT_NE(p1, p2);
  ASSERT_NE(t1, t2);

  // We expect 3 processes: idle process, 2x pid 1.
  ASSERT_EQ(context.storage->process_table().row_count(), 3u);
  // We expect 5 threads: Invalid thread, 2x (main thread + sub thread).
  ASSERT_EQ(context.storage->thread_table().row_count(), 5u);
}

TEST_F(ProcessTrackerTest, UpdateThreadName) {
  auto name1 = context.storage->InternString("name1");
  auto name2 = context.storage->InternString("name2");
  auto name3 = context.storage->InternString("name3");

  auto utid = context.process_tracker->GetOrCreateThread(1);

  context.process_tracker->UpdateThreadName(utid, name1,
                                            ThreadNamePriority::kFtrace);
  ASSERT_EQ(context.storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.storage->thread_table()[1].name(), name1);

  context.process_tracker->UpdateThreadName(utid, name2,
                                            ThreadNamePriority::kProcessTree);
  // The priority is higher: the name should change.
  ASSERT_EQ(context.storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.storage->thread_table()[1].name(), name2);

  context.process_tracker->UpdateThreadName(utid, name3,
                                            ThreadNamePriority::kFtrace);
  // The priority is lower: the name should stay the same.
  ASSERT_EQ(context.storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.storage->thread_table()[1].name(), name2);
}

TEST_F(ProcessTrackerTest, SetStartTsIfUnset) {
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/std::nullopt, 0u, 123, kNullStringId,
      ThreadNamePriority::kFtrace);
  context.process_tracker->SetStartTsIfUnset(upid, 1000);
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);

  context.process_tracker->SetStartTsIfUnset(upid, 3000);
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
}

TEST_F(ProcessTrackerTest, PidReuseAfterExplicitEnd) {
  UniquePid upid = context.process_tracker->GetOrCreateProcess(123);
  context.process_tracker->EndThread(100, 123);

  UniquePid reuse = context.process_tracker->GetOrCreateProcess(123);
  ASSERT_NE(upid, reuse);
}

TEST_F(ProcessTrackerTest, TidReuseAfterExplicitEnd) {
  UniqueTid utid = context.process_tracker->UpdateThread(123, 123);
  context.process_tracker->EndThread(100, 123);

  UniqueTid reuse = context.process_tracker->UpdateThread(123, 123);
  ASSERT_NE(utid, reuse);

  UniqueTid reuse_again = context.process_tracker->UpdateThread(123, 123);
  ASSERT_EQ(reuse, reuse_again);
}

TEST_F(ProcessTrackerTest, EndThreadAfterProcessEnd) {
  context.process_tracker->StartNewProcess(
      100, std::nullopt, 123, kNullStringId, ThreadNamePriority::kFtrace);
  context.process_tracker->UpdateThread(124, 123);

  context.process_tracker->EndThread(200, 123);
  context.process_tracker->EndThread(201, 124);

  // We expect two processes: the idle process and 123.
  ASSERT_EQ(context.storage->process_table().row_count(), 2u);

  // We expect three theads: the idle thread, 123 and 124.
  ASSERT_EQ(context.storage->thread_table().row_count(), 3u);
}

TEST_F(ProcessTrackerTest, UpdateTrustedPid) {
  context.process_tracker->UpdateTrustedPid(/*trusted_pid=*/123, /*uuid=*/1001);
  context.process_tracker->UpdateTrustedPid(/*trusted_pid=*/456, /*uuid=*/1002);

  ASSERT_EQ(context.process_tracker->GetTrustedPid(1001).value(), 123u);
  ASSERT_EQ(context.process_tracker->GetTrustedPid(1002).value(), 456u);

  // PID reuse. Multiple track UUIDs map to the same trusted_pid.
  context.process_tracker->UpdateTrustedPid(/*trusted_pid=*/123, /*uuid=*/1003);
  ASSERT_EQ(context.process_tracker->GetTrustedPid(1001).value(), 123u);
  ASSERT_EQ(context.process_tracker->GetTrustedPid(1003).value(), 123u);
}

TEST_F(ProcessTrackerTest, NamespacedProcessesAndThreads) {
  context.process_tracker->UpdateNamespacedProcess(/*pid=*/1001,
                                                   /*nspid=*/{1001, 190, 1});
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/1001, /*tid=*/1002,
      /*nstid=*/{1002, 192, 2}));
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(1001, 1003,
                                                              {1003, 193, 3}));

  context.process_tracker->UpdateNamespacedProcess(/*pid=*/1023,
                                                   /*nspid=*/{1023, 201, 21});
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/1023, /*tid=*/1026, {1026, 196, 26}));
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/1023, /*tid=*/1027, {1027, 197, 27}));

  context.process_tracker->UpdateNamespacedProcess(/*pid=*/1024,
                                                   /*nspid=*/{1024, 202, 22});
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/1024, /*tid=*/1028,
      /*nstid=*/{1028, 198, 28}));
  ASSERT_TRUE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/1024, /*tid=*/1029,
      /*nstid=*/{1029, 198, 29}));

  // Don't resolve if the process/thread isn't namespaced.
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(2001, 2002),
            std::nullopt);

  // Resolve from namespace-local PID to root-level PID.
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1001, 1).value(),
            1001u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1023, 21).value(),
            1023u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1024, 22).value(),
            1024u);

  // Resolve from namespace-local TID to root-level TID.
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1001, 2).value(),
            1002u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1001, 3).value(),
            1003u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1023, 26).value(),
            1026u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1023, 27).value(),
            1027u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1024, 28).value(),
            1028u);
  ASSERT_EQ(context.process_tracker->ResolveNamespacedTid(1024, 29).value(),
            1029u);
}

TEST_F(ProcessTrackerTest, EndProcessWithMainThread) {
  context.process_tracker->StartNewProcess(
      1000, std::nullopt, 123, kNullStringId, ThreadNamePriority::kFtrace);
  context.process_tracker->EndProcess(2000, 123);

  ASSERT_EQ(context.storage->process_table()[1].end_ts(), 2000);
  ASSERT_EQ(context.storage->thread_table()[1].end_ts(), 2000);

  // PID should be freed: creating a new process should yield a new upid.
  auto new_upid = context.process_tracker->GetOrCreateProcess(123);
  ASSERT_NE(new_upid, UniquePid{1});
}

TEST_F(ProcessTrackerTest, EndProcessWithoutMainThread) {
  auto upid = context.process_tracker->StartNewProcessWithoutMainThread(
      1000, std::nullopt, 123, kNullStringId,
      ThreadNamePriority::kGenericKernelTask);
  context.process_tracker->EndProcess(2000, 123);

  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);
  // No main thread should exist.
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(123).has_value());

  // PID should be freed: creating a new process should yield a new upid.
  auto new_upid = context.process_tracker->GetOrCreateProcess(123);
  ASSERT_NE(new_upid, upid);
}

TEST_F(ProcessTrackerTest, EndProcessNonExistent) {
  // Ending a process that was never tracked should not crash.
  context.process_tracker->EndProcess(1000, 999);
}

TEST_F(ProcessTrackerTest, EndProcessThenPidReuse) {
  auto upid1 = context.process_tracker->StartNewProcessWithoutMainThread(
      1000, std::nullopt, 42, kNullStringId,
      ThreadNamePriority::kGenericKernelTask);
  context.process_tracker->EndProcess(2000, 42);

  auto upid2 = context.process_tracker->StartNewProcessWithoutMainThread(
      3000, std::nullopt, 42, kNullStringId,
      ThreadNamePriority::kGenericKernelTask);

  ASSERT_NE(upid1, upid2);
  ASSERT_EQ(context.storage->process_table()[upid1].end_ts(), 2000);
  ASSERT_EQ(context.storage->process_table()[upid2].start_ts(), 3000);
  ASSERT_FALSE(context.storage->process_table()[upid2].end_ts().has_value());
}

// ---------------------------------------------------------------------------
// Tests simulating mixed ftrace and Android framework track event interactions.
// These mirror the ProcessTracker calls made by the parsing overrides in
// track_event_parser.cc for AndroidProcessStartEvent and
// AndroidBinderDiedEvent.
// ---------------------------------------------------------------------------

// Ftrace creates the process first (task_newtask). Then the Android framework
// start event arrives and finds the existing upid via UpidForPid. It should
// reuse the same upid and only fill in the uid.
TEST_F(ProcessTrackerTest, FtraceStartThenAndroidStart) {
  auto ftrace_name = context.storage->InternString("com.app");
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/100, ftrace_name,
      ThreadNamePriority::kFtrace);

  // Simulate AndroidProcessStartEvent: UpidForPid finds the process.
  auto existing = context.process_tracker->UpidForPid(100);
  ASSERT_TRUE(existing.has_value());
  ASSERT_EQ(*existing, upid);

  // Set uid (Android framework provides this).
  context.process_tracker->SetProcessUid(*existing, 10042);

  // Verify: same upid, ftrace name preserved, uid set.
  ASSERT_EQ(context.storage->process_table()[upid].start_ts(), 1000);
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid].name()),
            "com.app");
  ASSERT_EQ(*context.storage->process_table()[upid].uid(), 10042u);
  ASSERT_EQ(*context.storage->process_table()[upid].android_appid(), 10042u);
  // Main thread should still exist from ftrace.
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(100).has_value());
}

// Android framework start event arrives before ftrace task_newtask (e.g. due
// to timestamp ordering). Ftrace should take full precedence: StartNewProcess
// erases the old pid mapping and creates a fresh upid with a main thread.
TEST_F(ProcessTrackerTest, AndroidStartThenFtraceStart) {
  auto android_name = context.storage->InternString("com.app.android");
  auto android_upid = context.process_tracker->StartNewProcessWithoutMainThread(
      /*timestamp=*/900, std::nullopt, /*pid=*/100, android_name,
      ThreadNamePriority::kOther);
  context.process_tracker->SetProcessUid(android_upid, 10042);

  // Ftrace task_newtask arrives later with higher-priority name.
  auto ftrace_name = context.storage->InternString("com.app.ftrace");
  auto ftrace_upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/100, ftrace_name,
      ThreadNamePriority::kFtrace);

  // Ftrace creates a new upid (StartNewProcess erases old pid mapping).
  ASSERT_NE(android_upid, ftrace_upid);
  // The current pid mapping points to the ftrace-created process.
  auto current = context.process_tracker->UpidForPid(100);
  ASSERT_TRUE(current.has_value());
  ASSERT_EQ(*current, ftrace_upid);
  // Ftrace process has its own name and timestamp.
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[ftrace_upid].name()),
            "com.app.ftrace");
  ASSERT_EQ(context.storage->process_table()[ftrace_upid].start_ts(), 1000);
  // Main thread exists for ftrace-created process.
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(100).has_value());
}

// Ftrace creates the process, then Android BinderDied ends it. Verifies
// that end_ts is set and uid/name from the death event are populated.
TEST_F(ProcessTrackerTest, FtraceStartAndroidDeath) {
  auto ftrace_name = context.storage->InternString("com.app");
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/100, ftrace_name,
      ThreadNamePriority::kFtrace);

  // Simulate AndroidBinderDiedEvent: populate uid/name, then end.
  auto opt_upid = context.process_tracker->UpidForPid(100);
  ASSERT_TRUE(opt_upid.has_value());
  context.process_tracker->SetProcessUid(*opt_upid, 10042);
  // Name update at kOther priority should NOT override ftrace's kSystem name.
  auto death_name = context.storage->InternString("com.app.death");
  context.process_tracker->UpdateProcessName(*opt_upid, death_name,
                                             ProcessNamePriority::kOther);
  context.process_tracker->EndProcess(2000, 100);

  // Ftrace name preserved (kSystem > kOther).
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid].name()),
            "com.app");
  // end_ts and uid are set.
  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);
  ASSERT_EQ(*context.storage->process_table()[upid].uid(), 10042u);
  // pid is freed.
  ASSERT_FALSE(context.process_tracker->UpidForPid(100).has_value());
}

// Ftrace creates and ends the process (sched_process_free). Then Android
// BinderDied arrives for the same pid. The process is already gone from the
// pid map, so BinderDied should be a no-op: no crash, no new process created.
TEST_F(ProcessTrackerTest, FtraceFullLifecycleThenAndroidDeath) {
  auto ftrace_name = context.storage->InternString("com.app");
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/100, ftrace_name,
      ThreadNamePriority::kFtrace);
  // Ftrace sched_process_free: EndThread for the main thread ends the process.
  context.process_tracker->EndThread(2000, 100);

  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);
  ASSERT_FALSE(context.process_tracker->UpidForPid(100).has_value());

  // Android BinderDied arrives late. UpidForPid returns nullopt, so uid/name
  // update is skipped. EndProcess is a no-op.
  auto opt_upid = context.process_tracker->UpidForPid(100);
  ASSERT_FALSE(opt_upid.has_value());
  context.process_tracker->EndProcess(2500, 100);

  // Original end_ts unchanged, no new processes created.
  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);
  ASSERT_EQ(context.storage->process_table().row_count(), 2u);
}

// Android-only lifecycle: no ftrace events at all. Start event creates the
// process, BinderDied ends it. Then PID is reused for a second process.
TEST_F(ProcessTrackerTest, AndroidOnlyLifecycleWithPidReuse) {
  auto name1 = context.storage->InternString("com.first");
  auto upid1 = context.process_tracker->StartNewProcessWithoutMainThread(
      /*timestamp=*/1000, std::nullopt, /*pid=*/200, name1,
      ThreadNamePriority::kOther);
  context.process_tracker->SetProcessUid(upid1, 10001);

  // BinderDied ends the first process.
  context.process_tracker->EndProcess(2000, 200);
  ASSERT_EQ(context.storage->process_table()[upid1].end_ts(), 2000);

  // PID 200 is reused for a second process.
  auto name2 = context.storage->InternString("com.second");
  auto upid2 = context.process_tracker->StartNewProcessWithoutMainThread(
      /*timestamp=*/3000, std::nullopt, /*pid=*/200, name2,
      ThreadNamePriority::kOther);
  context.process_tracker->SetProcessUid(upid2, 10002);

  // Two distinct upids for the same pid.
  ASSERT_NE(upid1, upid2);
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid1].name()),
            "com.first");
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid2].name()),
            "com.second");
  ASSERT_EQ(*context.storage->process_table()[upid1].uid(), 10001u);
  ASSERT_EQ(*context.storage->process_table()[upid2].uid(), 10002u);
  // First process ended, second still alive.
  ASSERT_EQ(context.storage->process_table()[upid1].end_ts(), 2000);
  ASSERT_FALSE(context.storage->process_table()[upid2].end_ts().has_value());
}

// Ftrace creates an initial process snapshot. The process dies via Android
// BinderDied. A new process with the same PID starts via Android start event.
// This is the primary use case: ftrace provides the initial snapshot, Android
// framework handles subsequent lifecycle.
TEST_F(ProcessTrackerTest, FtraceInitialSnapshotThenAndroidLifecycle) {
  // Phase 1: ftrace captures the initial snapshot.
  auto ftrace_name = context.storage->InternString("com.app");
  auto upid1 = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/300, ftrace_name,
      ThreadNamePriority::kFtrace);

  // Phase 2: Android BinderDied kills the process.
  auto opt = context.process_tracker->UpidForPid(300);
  ASSERT_TRUE(opt.has_value());
  context.process_tracker->SetProcessUid(*opt, 10050);
  context.process_tracker->EndProcess(5000, 300);

  ASSERT_EQ(context.storage->process_table()[upid1].end_ts(), 5000);
  ASSERT_EQ(*context.storage->process_table()[upid1].uid(), 10050u);

  // Phase 3: Android ProcessStartEvent starts a new process with same PID.
  // UpidForPid returns nullopt (pid was freed), so a new process is created.
  ASSERT_FALSE(context.process_tracker->UpidForPid(300).has_value());
  auto new_name = context.storage->InternString("com.app.v2");
  auto upid2 = context.process_tracker->StartNewProcessWithoutMainThread(
      /*timestamp=*/6000, std::nullopt, /*pid=*/300, new_name,
      ThreadNamePriority::kOther);
  context.process_tracker->SetProcessUid(upid2, 10051);

  // Verify two distinct process entries.
  ASSERT_NE(upid1, upid2);
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid1].name()),
            "com.app");
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid2].name()),
            "com.app.v2");
  ASSERT_EQ(context.storage->process_table()[upid2].start_ts(), 6000);
  ASSERT_FALSE(context.storage->process_table()[upid2].end_ts().has_value());
}

// Ftrace creates a process with a name. Android start event finds it and
// tries to set uid. Verify that the ftrace name (kSystem priority) is never
// overwritten by a lower-priority source.
TEST_F(ProcessTrackerTest, FtraceNameNotOverwrittenByAndroid) {
  auto ftrace_name = context.storage->InternString("ftrace_name");
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/400, ftrace_name,
      ThreadNamePriority::kFtrace);

  // Android start event finds the process and tries to update the name
  // at kOther priority (simulating what would happen if code were changed
  // to always update name).
  auto android_name = context.storage->InternString("android_name");
  context.process_tracker->UpdateProcessName(upid, android_name,
                                             ProcessNamePriority::kOther);

  // Ftrace name survives (kSystem > kOther).
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid].name()),
            "ftrace_name");

  // Even kTrackDescriptor can't override kSystem.
  auto td_name = context.storage->InternString("track_descriptor_name");
  context.process_tracker->UpdateProcessName(
      upid, td_name, ProcessNamePriority::kTrackDescriptor);
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid].name()),
            "ftrace_name");

  // Only kSystem can override kSystem (same priority is allowed).
  auto system_name = context.storage->InternString("system_name");
  context.process_tracker->UpdateProcessName(upid, system_name,
                                             ProcessNamePriority::kSystem);
  ASSERT_EQ(context.storage->GetString(
                *context.storage->process_table()[upid].name()),
            "system_name");
}

// Both ftrace EndThread and Android EndProcess fire for the same process.
// The second one should be a no-op. Verifies idempotent ending.
TEST_F(ProcessTrackerTest, DoubleEndFtraceThenAndroid) {
  auto upid = context.process_tracker->StartNewProcess(
      /*timestamp=*/1000, std::nullopt, /*pid=*/500, kNullStringId,
      ThreadNamePriority::kFtrace);

  // Ftrace sched_process_free.
  context.process_tracker->EndThread(2000, 500);
  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);

  // Android BinderDied arrives later. EndProcess is a no-op.
  context.process_tracker->EndProcess(2500, 500);

  // end_ts unchanged from ftrace.
  ASSERT_EQ(context.storage->process_table()[upid].end_ts(), 2000);
}

// Android BinderDied arrives for a process that was never seen by any source.
// EndProcess should be a safe no-op.
TEST_F(ProcessTrackerTest, AndroidDeathForUnknownProcess) {
  // UpidForPid returns nullopt.
  ASSERT_FALSE(context.process_tracker->UpidForPid(999).has_value());

  // EndProcess should not crash or create any entries.
  auto row_count_before = context.storage->process_table().row_count();
  context.process_tracker->EndProcess(1000, 999);
  ASSERT_EQ(context.storage->process_table().row_count(), row_count_before);
}

TEST_F(ProcessTrackerTest, NamespacedThreadMissingProcess) {
  // Try to update a namespaced thread without first registering the process.
  // This should fail and return false.
  ASSERT_FALSE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/9999, /*tid=*/10000, /*nstid=*/{10000, 1}));

  // The import error stat should be incremented by the caller in production.
  // In this test, we just verify the function returns false.
}

}  // namespace
}  // namespace perfetto::trace_processor
