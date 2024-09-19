/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/ftrace_sched_event_tracker.h"
#include <cstdint>
#include <memory>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class SchedEventTrackerTest : public ::testing::Test {
 public:
  SchedEventTrackerTest() {
    context.storage = std::make_shared<TraceStorage>();
    context.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context.storage.get());
    context.args_tracker = std::make_unique<ArgsTracker>(&context);
    context.event_tracker = std::make_unique<EventTracker>(&context);
    context.process_tracker = std::make_unique<ProcessTracker>(&context);
    context.machine_tracker = std::make_unique<MachineTracker>(&context, 0);
    context.cpu_tracker = std::make_unique<CpuTracker>(&context);
    context.sched_event_tracker = std::make_unique<SchedEventTracker>(&context);
    sched_tracker = FtraceSchedEventTracker::GetOrCreate(&context);
  }

 protected:
  TraceProcessorContext context;
  FtraceSchedEventTracker* sched_tracker;
};

TEST_F(SchedEventTrackerTest, InsertSecondSched) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  uint32_t pid_1 = 2;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  int32_t prio = 1024;

  sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, kCommProc2, prio,
                                 prev_state, pid_2, kCommProc1, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 1ul);

  sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, kCommProc1, prio,
                                 prev_state, pid_1, kCommProc2, prio);

  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 2ul);

  const auto& sched = context.storage->sched_slice_table();
  ASSERT_EQ(sched[0].ts(), timestamp);
  ASSERT_EQ(context.storage->thread_table()[1].start_ts(), std::nullopt);

  auto name =
      context.storage->GetString(*context.storage->thread_table()[1].name());
  ASSERT_STREQ(name.c_str(), kCommProc1);
  ASSERT_EQ(context.storage->sched_slice_table()[0].utid(), 1u);
  ASSERT_EQ(context.storage->sched_slice_table()[0].dur(), 1);
}

TEST_F(SchedEventTrackerTest, InsertThirdSched_SameThread) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  int32_t prio = 1024;

  sched_tracker->PushSchedSwitch(cpu, timestamp, /*prev_pid=*/4, kCommProc2,
                                 prio, prev_state,
                                 /*tid=*/2, kCommProc1, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 1u);

  sched_tracker->PushSchedSwitch(cpu, timestamp + 1, /*prev_pid=*/2, kCommProc1,
                                 prio, prev_state,
                                 /*tid=*/4, kCommProc2, prio);
  sched_tracker->PushSchedSwitch(cpu, timestamp + 11, /*prev_pid=*/4,
                                 kCommProc2, prio, prev_state,
                                 /*tid=*/2, kCommProc1, prio);
  sched_tracker->PushSchedSwitch(cpu, timestamp + 31, /*tid=*/2, kCommProc1,
                                 prio, prev_state,
                                 /*tid=*/4, kCommProc2, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 4ul);

  ASSERT_EQ(context.storage->sched_slice_table()[0].ts(), timestamp);
  ASSERT_EQ(context.storage->thread_table()[1].start_ts(), std::nullopt);
  ASSERT_EQ(context.storage->sched_slice_table()[0].dur(), 1u);
  ASSERT_EQ(context.storage->sched_slice_table()[1].dur(), 11u - 1u);
  ASSERT_EQ(context.storage->sched_slice_table()[2].dur(), 31u - 11u);
  ASSERT_EQ(context.storage->sched_slice_table()[0].utid(),
            context.storage->sched_slice_table()[2].utid());
}

TEST_F(SchedEventTrackerTest, UpdateThreadMatch) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  int32_t prio = 1024;

  sched_tracker->PushSchedSwitch(cpu, timestamp, /*tid=*/1, kCommProc2, prio,
                                 prev_state,
                                 /*tid=*/4, kCommProc1, prio);
  sched_tracker->PushSchedSwitch(cpu, timestamp + 1, /*tid=*/4, kCommProc1,
                                 prio, prev_state,
                                 /*tid=*/1, kCommProc2, prio);

  context.process_tracker->SetProcessMetadata(2, std::nullopt, "test",
                                              base::StringView());
  context.process_tracker->UpdateThread(4, 2);

  ASSERT_EQ(context.storage->thread_table()[1].tid(), 4u);
  ASSERT_EQ(context.storage->thread_table()[1].upid().value(), 1u);
  ASSERT_EQ(context.storage->process_table()[1].pid(), 2u);
  ASSERT_EQ(context.storage->process_table()[1].start_ts(), std::nullopt);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
