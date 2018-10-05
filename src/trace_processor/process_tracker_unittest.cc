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

#include "src/trace_processor/process_tracker.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/trace_processor.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class ProcessTrackerTest : public ::testing::Test {
 public:
  ProcessTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.process_tracker.reset(new ProcessTracker(&context));
    context.sched_tracker.reset(new SchedTracker(&context));
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(ProcessTrackerTest, PushProcess) {
  TraceStorage storage;
  context.process_tracker->UpdateProcess(1, "test");
  auto pair_it = context.process_tracker->UpidsForPid(1);
  ASSERT_EQ(pair_it.first->second, 1);
}

TEST_F(ProcessTrackerTest, PushTwoProcessEntries_SamePidAndName) {
  context.process_tracker->UpdateProcess(1, "test");
  context.process_tracker->UpdateProcess(1, "test");
  auto pair_it = context.process_tracker->UpidsForPid(1);
  ASSERT_EQ(pair_it.first->second, 1);
  ASSERT_EQ(++pair_it.first, pair_it.second);
}

TEST_F(ProcessTrackerTest, PushTwoProcessEntries_DifferentPid) {
  context.process_tracker->UpdateProcess(1, "test");
  context.process_tracker->UpdateProcess(3, "test");
  auto pair_it = context.process_tracker->UpidsForPid(1);
  ASSERT_EQ(pair_it.first->second, 1);
  auto second_pair_it = context.process_tracker->UpidsForPid(3);
  ASSERT_EQ(second_pair_it.first->second, 2);
}

TEST_F(ProcessTrackerTest, AddProcessEntry_CorrectName) {
  context.process_tracker->UpdateProcess(1, "test");
  ASSERT_EQ(context.storage->GetString(context.storage->GetProcess(1).name_id),
            "test");
}

TEST_F(ProcessTrackerTest, UpdateThreadMatch) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";

  context.sched_tracker->PushSchedSwitch(cpu, timestamp, /*tid=*/1, prev_state,
                                         /*tid=*/4, kCommProc1);
  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, /*tid=*/4,
                                         prev_state, /*tid=*/1, kCommProc2);

  context.process_tracker->UpdateProcess(2, "test");
  context.process_tracker->UpdateThread(4, 2);

  TraceStorage::Thread thread = context.storage->GetThread(/*utid=*/1);
  TraceStorage::Process process = context.storage->GetProcess(/*utid=*/1);

  ASSERT_EQ(thread.tid, 4);
  ASSERT_EQ(thread.upid, 1);
  ASSERT_EQ(process.pid, 2);
  ASSERT_EQ(process.start_ns, timestamp);
}

TEST_F(ProcessTrackerTest, UpdateThreadCreate) {
  context.process_tracker->UpdateThread(12, 2);

  TraceStorage::Thread thread = context.storage->GetThread(1);

  ASSERT_EQ(context.storage->thread_count(), 1);
  auto tid_it = context.process_tracker->UtidsForTid(12);
  ASSERT_NE(tid_it.first, tid_it.second);
  ASSERT_EQ(thread.upid, 1);
  auto pid_it = context.process_tracker->UpidsForPid(2);
  ASSERT_NE(pid_it.first, pid_it.second);
  ASSERT_EQ(context.storage->process_count(), 1);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
