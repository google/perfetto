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

#include "src/trace_processor/sched_tracker.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/trace_processor.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class SchedTrackerTest : public ::testing::Test {
 public:
  SchedTrackerTest() {
    context.process_tracker.reset(new ProcessTracker(&context));
    context.sched_tracker.reset(new SchedTracker(&context));
    context.storage.reset(new TraceStorage());
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(SchedTrackerTest, InsertSecondSched) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;

  const auto& timestamps = context.storage->SlicesForCpu(cpu).start_ns();
  context.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                         kCommProc1, sizeof(kCommProc1) - 1,
                                         pid_2);
  ASSERT_EQ(timestamps.size(), 0);

  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, prev_state,
                                         kCommProc2, sizeof(kCommProc2) - 1,
                                         pid_1);

  ASSERT_EQ(timestamps.size(), 1ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, timestamp);
  ASSERT_EQ(std::string(context.storage->GetString(
                context.storage->GetThread(1).name_id)),
            "process1");
  ASSERT_EQ(context.storage->SlicesForCpu(cpu).utids().front(), 1);
}

TEST_F(SchedTrackerTest, InsertThirdSched_SameThread) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t pid_1 = 2;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;

  const auto& timestamps = context.storage->SlicesForCpu(cpu).start_ns();
  context.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                         kCommProc1, sizeof(kCommProc1) - 1,
                                         pid_1);
  ASSERT_EQ(timestamps.size(), 0);

  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_1, prev_state,
                                         kCommProc1, sizeof(kCommProc1) - 1,
                                         pid_2);
  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 2, pid_2, prev_state,
                                         kCommProc2, sizeof(kCommProc2) - 1,
                                         pid_1);

  ASSERT_EQ(timestamps.size(), 2ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, timestamp);
  ASSERT_EQ(context.storage->SlicesForCpu(cpu).utids().at(0),
            context.storage->SlicesForCpu(cpu).utids().at(1));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
