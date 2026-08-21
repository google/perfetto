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
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
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
    context.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context.storage.get());
    context.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context.storage.get());
    context.machine_tracker =
        std::make_unique<MachineTracker>(&context, kDefaultMachineId);
    context.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context.stats_tracker = std::make_unique<StatsTracker>(&context);
    context.import_logs_tracker.reset(
        new ImportLogsTracker(&context, TraceId{1}));
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

TEST_F(ProcessTrackerTest, SetProcessParent) {
  UniquePid pupid1 = context.process_tracker->GetOrCreateProcess(123);
  UniquePid pupid2 = context.process_tracker->GetOrCreateProcess(234);
  UniquePid upid = context.process_tracker->GetOrCreateProcess(345);

  context.process_tracker->SetProcessParent(upid, pupid1);
  ASSERT_EQ(pupid1, context.storage->process_table()[upid].parent_upid());

  // A changed parent is treated as the same process being reparented: the
  // original parent is kept.
  context.process_tracker->SetProcessParent(upid, pupid2);
  ASSERT_EQ(pupid1, context.storage->process_table()[upid].parent_upid());
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

TEST_F(ProcessTrackerTest, NamespacedThreadMissingProcess) {
  // Try to update a namespaced thread without first registering the process.
  // This should fail and return false.
  ASSERT_FALSE(context.process_tracker->UpdateNamespacedThread(
      /*pid=*/9999, /*tid=*/10000, /*nstid=*/{10000, 1}));

  // The import error stat should be incremented by the caller in production.
  // In this test, we just verify the function returns false.
}

// Characterization tests pinning the tid -> utid lookup semantics (bare and
// pid-qualified lookups, tid/pid recycling, resurrection). A failure here is a
// real behavioral change, not a test to be "fixed".

// An unknown tid resolves to nothing and is not created by GetThreadOrNull.
TEST_F(ProcessTrackerTest, BareLookupUnknownTidIsNull) {
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(42).has_value());
  // The table still only has the reserved idle thread (utid 0).
  ASSERT_EQ(context.storage->thread_table().row_count(), 1u);
}

// A single created thread resolves to itself.
TEST_F(ProcessTrackerTest, BareLookupSingleThread) {
  UniqueTid t = context.process_tracker->GetOrCreateThread(/*tid=*/42);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(42), t);
  // Idempotent: a second GetOrCreate returns the same utid, no new row.
  ASSERT_EQ(context.process_tracker->GetOrCreateThread(42), t);
  ASSERT_EQ(context.storage->thread_table().row_count(), 2u);
}

// Starting a second incarnation of the same tid shadows the older one: bare
// lookups return the newest live incarnation.
TEST_F(ProcessTrackerTest, BareLookupReturnsNewestIncarnation) {
  UniqueTid t1 = context.process_tracker->StartNewThread(1000, /*tid=*/42);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(42), t1);

  UniqueTid t2 = context.process_tracker->StartNewThread(2000, /*tid=*/42);
  ASSERT_NE(t1, t2);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(42), t2);
  ASSERT_EQ(context.process_tracker->GetOrCreateThread(42), t2);

  // Both incarnations still exist as rows in the table (history preserved).
  ASSERT_EQ(context.storage->thread_table()[t1].tid(), 42);
  ASSERT_EQ(context.storage->thread_table()[t2].tid(), 42);
}

// A bare-created thread (no known parent process) is considered alive.
TEST_F(ProcessTrackerTest, BareThreadIsAliveWithoutProcess) {
  UniqueTid t = context.process_tracker->GetOrCreateThread(/*tid=*/42);
  ASSERT_FALSE(context.storage->thread_table()[t].upid().has_value());
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(t));
}

// After an explicit EndThread the tid no longer resolves; a fresh incarnation
// is minted on the next GetOrCreateThread, and the old row gets an end_ts.
TEST_F(ProcessTrackerTest, EndThreadRemovesFromBareLookupAndStampsEndTs) {
  UniqueTid t1 = context.process_tracker->GetOrCreateThread(/*tid=*/77);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(77), t1);

  context.process_tracker->EndThread(500, /*tid=*/77);
  ASSERT_EQ(context.storage->thread_table()[t1].end_ts(), 500);
  ASSERT_FALSE(context.process_tracker->IsThreadAlive(t1));
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(77).has_value());

  UniqueTid t2 = context.process_tracker->GetOrCreateThread(/*tid=*/77);
  ASSERT_NE(t1, t2);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(77), t2);
}

// EndThread for a tid that was never seen is a no-op: it must not create a
// thread (see b/193520421 - a free event after the process already ended).
TEST_F(ProcessTrackerTest, EndThreadOnUnknownTidIsNoOp) {
  size_t before = context.storage->thread_table().row_count();
  context.process_tracker->EndThread(500, /*tid=*/999);
  ASSERT_EQ(context.storage->thread_table().row_count(), before);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(999).has_value());
}

// Ending a non-main thread ends only the thread; the process stays alive and
// its main thread keeps resolving.
TEST_F(ProcessTrackerTest, EndNonMainThreadKeepsProcessAlive) {
  context.process_tracker->StartNewProcess(100, std::nullopt, /*pid=*/123,
                                           kNullStringId,
                                           ThreadNamePriority::kFtrace);
  UniqueTid sub =
      context.process_tracker->UpdateThread(/*tid=*/124, /*pid=*/123);

  context.process_tracker->EndThread(200, /*tid=*/124);
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(124).has_value());

  // Main thread (tid 123) of the still-alive process resolves fine.
  ASSERT_TRUE(context.process_tracker->GetThreadOrNull(123).has_value());
  UniquePid upid = *context.process_tracker->UpidForPidForTesting(123);
  ASSERT_FALSE(context.storage->process_table()[upid].end_ts().has_value());
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(sub) == false);
}

// Ending the main thread (tid == pid) ends the whole process, so its other
// threads stop resolving by bare tid.
TEST_F(ProcessTrackerTest, EndMainThreadEndsProcessAndOrphansThreads) {
  context.process_tracker->StartNewProcess(100, std::nullopt, /*pid=*/123,
                                           kNullStringId,
                                           ThreadNamePriority::kFtrace);
  UniqueTid sub =
      context.process_tracker->UpdateThread(/*tid=*/124, /*pid=*/123);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(124), sub);
  UniquePid upid = *context.process_tracker->UpidForPidForTesting(123);

  context.process_tracker->EndThread(200, /*tid=*/123);

  ASSERT_TRUE(context.storage->process_table()[upid].end_ts().has_value());
  // The sub-thread's process is dead -> it must not resolve and is not alive.
  ASSERT_FALSE(context.process_tracker->IsThreadAlive(sub));
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(124).has_value());
  // And the pid mapping was cleared, so GetOrCreateProcess re-mints.
  ASSERT_NE(context.process_tracker->GetOrCreateProcess(123), upid);
}

// EndThread on a worker thread after its process already ended must not
// resurrect the process or create rows (regression b/193520421).
TEST_F(ProcessTrackerTest, EndThreadAfterProcessEndCreatesNothing) {
  context.process_tracker->StartNewProcess(100, std::nullopt, /*pid=*/123,
                                           kNullStringId,
                                           ThreadNamePriority::kFtrace);
  context.process_tracker->UpdateThread(/*tid=*/124, /*pid=*/123);

  context.process_tracker->EndThread(200, /*tid=*/123);  // ends process
  size_t threads = context.storage->thread_table().row_count();
  size_t procs = context.storage->process_table().row_count();

  context.process_tracker->EndThread(201, /*tid=*/124);  // worker, late
  ASSERT_EQ(context.storage->thread_table().row_count(), threads);
  ASSERT_EQ(context.storage->process_table().row_count(), procs);
}

// GetOrCreateProcess is stable: repeated calls for the same pid return the
// same upid (no implicit recycle).
TEST_F(ProcessTrackerTest, GetOrCreateProcessIsStable) {
  UniquePid u = context.process_tracker->GetOrCreateProcess(/*pid=*/500);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(500), u);
  ASSERT_EQ(context.process_tracker->GetOrCreateProcess(500), u);
}

// A second StartNewProcess for a live pid recycles it: a new upid is minted,
// the old upid row is retained, and pids_ now points at the new upid.
TEST_F(ProcessTrackerTest, StartNewProcessRecyclesLivePid) {
  UniquePid p1 = context.process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/10, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniquePid p2 = context.process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/10, kNullStringId,
      ThreadNamePriority::kFtrace);
  ASSERT_NE(p1, p2);
  ASSERT_EQ(context.process_tracker->UpidForPidForTesting(10), p2);
  // p1 still exists as history.
  ASSERT_EQ(context.storage->process_table()[p1].pid(), 10);
}

// The crux case: implicit pid recycle (StartNewProcess again, no EndThread)
// must invalidate the *old* process's worker threads for bare lookups. This
// is what the lazy `pids_` check inside IsThreadAlive currently provides.
TEST_F(ProcessTrackerTest, ImplicitPidRecycleInvalidatesOldWorkerThreads) {
  context.process_tracker->StartNewProcess(std::nullopt, std::nullopt,
                                           /*pid=*/10, kNullStringId,
                                           ThreadNamePriority::kFtrace);
  UniqueTid worker =
      context.process_tracker->UpdateThread(/*tid=*/20, /*pid=*/10);
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(20), worker);
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(worker));

  // Recycle pid 10 into a brand new process without ending anything.
  context.process_tracker->StartNewProcess(std::nullopt, std::nullopt,
                                           /*pid=*/10, kNullStringId,
                                           ThreadNamePriority::kFtrace);

  // The old worker's process was superseded -> dead, must not resolve.
  ASSERT_FALSE(context.process_tracker->IsThreadAlive(worker));
  ASSERT_NE(context.process_tracker->GetThreadOrNull(20), worker);
}

// Full PidReuseWithoutStartAndEndThread shape: each recycle yields fresh
// utids/upids and the right row counts (mirrors the existing test, kept here
// for completeness with the lookup-focused asserts above).
TEST_F(ProcessTrackerTest, PidReuseMintsFreshThreadsAndProcesses) {
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
  // The latest worker for tid 2 is the one parented to the latest process.
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(2), t2);
}

// A bare-created thread (unknown process) is adopted by the first WithParent
// call rather than creating a new utid.
TEST_F(ProcessTrackerTest, WithParentAdoptsExistingBareThread) {
  UniqueTid bare = context.process_tracker->GetOrCreateThread(/*tid=*/600);
  ASSERT_FALSE(context.storage->thread_table()[bare].upid().has_value());

  UniquePid upid = context.process_tracker->GetOrCreateProcess(/*pid=*/700);
  UniqueTid adopted = context.process_tracker->GetOrCreateThreadWithParent(
      /*tid=*/600, upid, /*associate_main_threads=*/false);

  ASSERT_EQ(adopted, bare);
  ASSERT_EQ(context.storage->thread_table()[bare].upid(), upid);
}

// The same tid associated to two different parents in turn yields two distinct
// incarnations, and a pid-qualified lookup returns the matching one.
TEST_F(ProcessTrackerTest, WithParentDisambiguatesSameTidAcrossProcesses) {
  UniquePid upid_a = context.process_tracker->GetOrCreateProcess(/*pid=*/1000);
  UniquePid upid_b = context.process_tracker->GetOrCreateProcess(/*pid=*/2000);

  UniqueTid in_a = context.process_tracker->GetOrCreateThreadWithParent(
      /*tid=*/555, upid_a, false);
  UniqueTid in_b = context.process_tracker->GetOrCreateThreadWithParent(
      /*tid=*/555, upid_b, false);
  ASSERT_NE(in_a, in_b);

  // Re-resolving with each parent returns the matching incarnation.
  ASSERT_EQ(
      context.process_tracker->GetOrCreateThreadWithParent(555, upid_a, false),
      in_a);
  ASSERT_EQ(
      context.process_tracker->GetOrCreateThreadWithParent(555, upid_b, false),
      in_b);
}

// UpdateThread(tid==pid) marks the thread as the process main thread.
TEST_F(ProcessTrackerTest, UpdateThreadMainThreadFlag) {
  UniqueTid main =
      context.process_tracker->UpdateThread(/*tid=*/42, /*pid=*/42);
  UniqueTid worker =
      context.process_tracker->UpdateThread(/*tid=*/43, /*pid=*/42);
  ASSERT_TRUE(context.storage->thread_table()[main].is_main_thread().value());
  ASSERT_FALSE(
      context.storage->thread_table()[worker].is_main_thread().value());
}

//
// When the newest incarnation of a tid goes stale (its process was recycled)
// but an older one is still alive, a bare lookup falls back to the older one.
TEST_F(ProcessTrackerTest, ResurrectsOlderLiveIncarnationWhenNewestStale) {
  // Older incarnation A of tid 5 under a process (pid 1) that stays alive.
  UniquePid p1 = context.process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/1, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniqueTid a = context.process_tracker->UpdateThread(/*tid=*/5, /*pid=*/1);

  // Newer incarnation B of the same tid 5 under a different process (pid 2).
  context.process_tracker->StartNewProcess(std::nullopt, std::nullopt,
                                           /*pid=*/2, kNullStringId,
                                           ThreadNamePriority::kFtrace);
  UniqueTid b = context.process_tracker->UpdateThread(/*tid=*/5, /*pid=*/2);
  ASSERT_NE(a, b);
  // B is newest, so it wins the bare lookup right now.
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(5), b);

  // Recycle pid 2: B becomes stale, but A (pid 1) is still alive.
  context.process_tracker->StartNewProcess(std::nullopt, std::nullopt,
                                           /*pid=*/2, kNullStringId,
                                           ThreadNamePriority::kFtrace);
  ASSERT_FALSE(context.process_tracker->IsThreadAlive(b));
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(a));
  ASSERT_EQ(p1, *context.process_tracker->UpidForPidForTesting(1));

  // Falls back to the older live incarnation A.
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(5), a);
}

// Before declaring the idle process, tid 0 is an ordinary tid: it does not map
// to the reserved swapper utid and GetOrCreateThread mints a fresh utid.
TEST_F(ProcessTrackerTest, TidZeroIsOrdinaryBeforeIdleDeclared) {
  ASSERT_FALSE(context.process_tracker->GetThreadOrNull(0).has_value());
  UniqueTid t0 = context.process_tracker->GetOrCreateThread(/*tid=*/0);
  ASSERT_NE(t0, context.process_tracker->swapper_utid());
}

// After SetPidZeroIsUpidZeroIdleProcess, tid 0 resolves to the swapper utid.
TEST_F(ProcessTrackerTest, TidZeroResolvesToSwapperAfterIdleDeclared) {
  context.process_tracker->SetPidZeroIsUpidZeroIdleProcess();
  ASSERT_EQ(context.process_tracker->GetThreadOrNull(0),
            context.process_tracker->swapper_utid());
  ASSERT_EQ(context.process_tracker->GetOrCreateThread(0),
            context.process_tracker->swapper_utid());
}

// IsThreadAlive returns false once a thread's own end_ts is set, false once its
// process ends, and false once its process pid is recycled; true otherwise.
TEST_F(ProcessTrackerTest, IsThreadAliveTruthTable) {
  // (1) bare thread, no process -> alive.
  UniqueTid bare = context.process_tracker->GetOrCreateThread(/*tid=*/9001);
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(bare));

  // (2) thread with a live process -> alive.
  context.process_tracker->StartNewProcess(std::nullopt, std::nullopt,
                                           /*pid=*/30, kNullStringId,
                                           ThreadNamePriority::kFtrace);
  UniqueTid worker =
      context.process_tracker->UpdateThread(/*tid=*/31, /*pid=*/30);
  ASSERT_TRUE(context.process_tracker->IsThreadAlive(worker));

  // (3) explicit end -> dead.
  context.process_tracker->EndThread(1000, /*tid=*/31);
  ASSERT_FALSE(context.process_tracker->IsThreadAlive(worker));
}

}  // namespace
}  // namespace perfetto::trace_processor
