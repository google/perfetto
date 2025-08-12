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
using ::testing::Invoke;

class ProcessTrackerTest : public ::testing::Test {
 public:
  ProcessTrackerTest() {
    context.global_context->storage = std::make_shared<TraceStorage>();
    context.machine_context->machine_tracker =
        std::make_unique<MachineTracker>(&context, 0);
    context.global_context->global_args_tracker =
        std::make_unique<GlobalArgsTracker>(
            context.global_context->storage.get());
    context.trace_context->args_tracker =
        std::make_unique<ArgsTracker>(&context);
    context.machine_context->process_tracker =
        std::make_unique<ProcessTracker>(&context);
    context.trace_context->event_tracker =
        std::make_unique<EventTracker>(&context);
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(ProcessTrackerTest, PushProcess) {
  context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", base::StringView());
  auto opt_upid =
      context.machine_context->process_tracker->UpidForPidForTesting(1);
  ASSERT_EQ(opt_upid.value_or(-1), 1u);
}

TEST_F(ProcessTrackerTest, GetOrCreateNewProcess) {
  auto upid = context.machine_context->process_tracker->GetOrCreateProcess(123);
  ASSERT_EQ(context.machine_context->process_tracker->GetOrCreateProcess(123),
            upid);
}

TEST_F(ProcessTrackerTest, StartNewProcess) {
  auto upid = context.machine_context->process_tracker->StartNewProcess(
      1000, 0u, 123, kNullStringId, ThreadNamePriority::kFtrace);
  ASSERT_EQ(context.machine_context->process_tracker->GetOrCreateProcess(123),
            upid);
  ASSERT_EQ(context.global_context->storage->process_table()[upid].start_ts(),
            1000);
}

TEST_F(ProcessTrackerTest, PushTwoProcessEntries_SamePidAndName) {
  context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", base::StringView());
  context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", base::StringView());
  auto opt_upid =
      context.machine_context->process_tracker->UpidForPidForTesting(1);
  ASSERT_EQ(opt_upid.value_or(-1), 1u);
}

TEST_F(ProcessTrackerTest, PushTwoProcessEntries_DifferentPid) {
  context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", base::StringView());
  context.machine_context->process_tracker->SetProcessMetadata(
      3, std::nullopt, "test", base::StringView());
  auto opt_upid =
      context.machine_context->process_tracker->UpidForPidForTesting(1);
  ASSERT_EQ(opt_upid.value_or(-1), 1u);
  opt_upid = context.machine_context->process_tracker->UpidForPidForTesting(3);
  ASSERT_EQ(opt_upid.value_or(-1), 2u);
}

TEST_F(ProcessTrackerTest, AddProcessEntry_CorrectName) {
  context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", base::StringView());
  auto name = context.global_context->storage->process_table()[1].name();
  ASSERT_EQ(context.global_context->storage->GetString(*name), "test");
}

TEST_F(ProcessTrackerTest, UpdateThreadCreate) {
  context.machine_context->process_tracker->UpdateThread(12, 2);

  // We expect 3 threads: Invalid thread, main thread for pid, tid 12.
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 3u);

  auto tid_it =
      context.machine_context->process_tracker->UtidsForTidForTesting(12);
  ASSERT_NE(tid_it.first, tid_it.second);
  ASSERT_EQ(context.global_context->storage->thread_table()[1].upid().value(),
            1u);
  auto opt_upid =
      context.machine_context->process_tracker->UpidForPidForTesting(2);
  ASSERT_TRUE(opt_upid.has_value());
  ASSERT_EQ(context.global_context->storage->process_table().row_count(), 2u);
}

TEST_F(ProcessTrackerTest, PidReuseWithoutStartAndEndThread) {
  UniquePid p1 = context.machine_context->process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/1, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniqueTid t1 = context.machine_context->process_tracker->UpdateThread(
      /*tid=*/2, /*pid=*/1);

  UniquePid p2 = context.machine_context->process_tracker->StartNewProcess(
      std::nullopt, std::nullopt, /*pid=*/1, kNullStringId,
      ThreadNamePriority::kFtrace);
  UniqueTid t2 = context.machine_context->process_tracker->UpdateThread(
      /*tid=*/2, /*pid=*/1);

  ASSERT_NE(p1, p2);
  ASSERT_NE(t1, t2);

  // We expect 3 processes: idle process, 2x pid 1.
  ASSERT_EQ(context.global_context->storage->process_table().row_count(), 3u);
  // We expect 5 threads: Invalid thread, 2x (main thread + sub thread).
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 5u);
}

TEST_F(ProcessTrackerTest, Cmdline) {
  UniquePid upid = context.machine_context->process_tracker->SetProcessMetadata(
      1, std::nullopt, "test", "cmdline blah");
  auto cmdline =
      *context.global_context->storage->process_table()[upid].cmdline();
  ASSERT_EQ(context.global_context->storage->GetString(cmdline),
            "cmdline blah");
}

TEST_F(ProcessTrackerTest, UpdateThreadName) {
  auto name1 = context.global_context->storage->InternString("name1");
  auto name2 = context.global_context->storage->InternString("name2");
  auto name3 = context.global_context->storage->InternString("name3");

  auto utid = context.machine_context->process_tracker->GetOrCreateThread(1);

  context.machine_context->process_tracker->UpdateThreadName(
      utid, name1, ThreadNamePriority::kFtrace);
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.global_context->storage->thread_table()[1].name(), name1);

  context.machine_context->process_tracker->UpdateThreadName(
      utid, name2, ThreadNamePriority::kProcessTree);
  // The priority is higher: the name should change.
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.global_context->storage->thread_table()[1].name(), name2);

  context.machine_context->process_tracker->UpdateThreadName(
      utid, name3, ThreadNamePriority::kFtrace);
  // The priority is lower: the name should stay the same.
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 2u);
  ASSERT_EQ(context.global_context->storage->thread_table()[1].name(), name2);
}

TEST_F(ProcessTrackerTest, SetStartTsIfUnset) {
  auto upid = context.machine_context->process_tracker->StartNewProcess(
      /*timestamp=*/std::nullopt, 0u, 123, kNullStringId,
      ThreadNamePriority::kFtrace);
  context.machine_context->process_tracker->SetStartTsIfUnset(upid, 1000);
  ASSERT_EQ(context.global_context->storage->process_table()[upid].start_ts(),
            1000);

  context.machine_context->process_tracker->SetStartTsIfUnset(upid, 3000);
  ASSERT_EQ(context.global_context->storage->process_table()[upid].start_ts(),
            1000);
}

TEST_F(ProcessTrackerTest, PidReuseAfterExplicitEnd) {
  UniquePid upid =
      context.machine_context->process_tracker->GetOrCreateProcess(123);
  context.machine_context->process_tracker->EndThread(100, 123);

  UniquePid reuse =
      context.machine_context->process_tracker->GetOrCreateProcess(123);
  ASSERT_NE(upid, reuse);
}

TEST_F(ProcessTrackerTest, TidReuseAfterExplicitEnd) {
  UniqueTid utid =
      context.machine_context->process_tracker->UpdateThread(123, 123);
  context.machine_context->process_tracker->EndThread(100, 123);

  UniqueTid reuse =
      context.machine_context->process_tracker->UpdateThread(123, 123);
  ASSERT_NE(utid, reuse);

  UniqueTid reuse_again =
      context.machine_context->process_tracker->UpdateThread(123, 123);
  ASSERT_EQ(reuse, reuse_again);
}

TEST_F(ProcessTrackerTest, EndThreadAfterProcessEnd) {
  context.machine_context->process_tracker->StartNewProcess(
      100, std::nullopt, 123, kNullStringId, ThreadNamePriority::kFtrace);
  context.machine_context->process_tracker->UpdateThread(124, 123);

  context.machine_context->process_tracker->EndThread(200, 123);
  context.machine_context->process_tracker->EndThread(201, 124);

  // We expect two processes: the idle process and 123.
  ASSERT_EQ(context.global_context->storage->process_table().row_count(), 2u);

  // We expect three theads: the idle thread, 123 and 124.
  ASSERT_EQ(context.global_context->storage->thread_table().row_count(), 3u);
}

TEST_F(ProcessTrackerTest, UpdateTrustedPid) {
  context.machine_context->process_tracker->UpdateTrustedPid(
      /*trusted_pid=*/123, /*uuid=*/1001);
  context.machine_context->process_tracker->UpdateTrustedPid(
      /*trusted_pid=*/456, /*uuid=*/1002);

  ASSERT_EQ(
      context.machine_context->process_tracker->GetTrustedPid(1001).value(),
      123u);
  ASSERT_EQ(
      context.machine_context->process_tracker->GetTrustedPid(1002).value(),
      456u);

  // PID reuse. Multiple track UUIDs map to the same trusted_pid.
  context.machine_context->process_tracker->UpdateTrustedPid(
      /*trusted_pid=*/123, /*uuid=*/1003);
  ASSERT_EQ(
      context.machine_context->process_tracker->GetTrustedPid(1001).value(),
      123u);
  ASSERT_EQ(
      context.machine_context->process_tracker->GetTrustedPid(1003).value(),
      123u);
}

TEST_F(ProcessTrackerTest, NamespacedProcessesAndThreads) {
  context.machine_context->process_tracker->UpdateNamespacedProcess(
      /*pid=*/1001,
      /*nspid=*/{1001, 190, 1});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      /*pid=*/1001, /*tid=*/1002,
      /*nstid=*/{1002, 192, 2});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      1001, 1003, {1003, 193, 3});

  context.machine_context->process_tracker->UpdateNamespacedProcess(
      /*pid=*/1023,
      /*nspid=*/{1023, 201, 21});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      /*pid=*/1023, /*tid=*/1026, {1026, 196, 26});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      /*pid=*/1023, /*tid=*/1027, {1027, 197, 27});

  context.machine_context->process_tracker->UpdateNamespacedProcess(
      /*pid=*/1024,
      /*nspid=*/{1024, 202, 22});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      /*pid=*/1024, /*tid=*/1028,
      /*nstid=*/{1028, 198, 28});
  context.machine_context->process_tracker->UpdateNamespacedThread(
      /*pid=*/1024, /*tid=*/1029,
      /*nstid=*/{1029, 198, 29});

  // Don't resolve if the process/thread isn't namespaced.
  ASSERT_EQ(context.machine_context->process_tracker->ResolveNamespacedTid(
                2001, 2002),
            std::nullopt);

  // Resolve from namespace-local PID to root-level PID.
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1001, 1)
          .value(),
      1001u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1023, 21)
          .value(),
      1023u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1024, 22)
          .value(),
      1024u);

  // Resolve from namespace-local TID to root-level TID.
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1001, 2)
          .value(),
      1002u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1001, 3)
          .value(),
      1003u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1023, 26)
          .value(),
      1026u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1023, 27)
          .value(),
      1027u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1024, 28)
          .value(),
      1028u);
  ASSERT_EQ(
      context.machine_context->process_tracker->ResolveNamespacedTid(1024, 29)
          .value(),
      1029u);
}

}  // namespace
}  // namespace perfetto::trace_processor
