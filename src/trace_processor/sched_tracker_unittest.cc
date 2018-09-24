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
    context.storage.reset(new TraceStorage());
    context.process_tracker.reset(new ProcessTracker(&context));
    context.sched_tracker.reset(new SchedTracker(&context));
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

  const auto& timestamps = context.storage->slices().start_ns();
  context.sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, prev_state,
                                         kCommProc1, pid_2);
  ASSERT_EQ(timestamps.size(), 0);

  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, prev_state,
                                         kCommProc2, pid_1);

  ASSERT_EQ(timestamps.size(), 1ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, timestamp);
  ASSERT_EQ(std::string(context.storage->GetString(
                context.storage->GetThread(1).name_id)),
            kCommProc2);
  ASSERT_EQ(context.storage->slices().utids().front(), 1);
}

TEST_F(SchedTrackerTest, InsertThirdSched_SameThread) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  uint32_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";

  const auto& timestamps = context.storage->slices().start_ns();
  context.sched_tracker->PushSchedSwitch(cpu, timestamp, /*tid=*/4, prev_state,
                                         kCommProc1,
                                         /*tid=*/2);
  ASSERT_EQ(timestamps.size(), 0);

  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 1, /*tid=*/2,
                                         prev_state, kCommProc1,
                                         /*tid=*/4);
  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 11, /*tid=*/4,
                                         prev_state, kCommProc2,
                                         /*tid=*/2);
  context.sched_tracker->PushSchedSwitch(cpu, timestamp + 31, /*tid=*/4,
                                         prev_state, kCommProc1,
                                         /*tid=*/2);

  ASSERT_EQ(timestamps.size(), 3ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, timestamp);
  ASSERT_EQ(context.storage->slices().durations().at(0), 1u);
  ASSERT_EQ(context.storage->slices().durations().at(1), 11u - 1u);
  ASSERT_EQ(context.storage->slices().durations().at(2), 31u - 11u);
  ASSERT_EQ(context.storage->slices().utids().at(0),
            context.storage->slices().utids().at(2));
}

TEST_F(SchedTrackerTest, CounterDuration) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  StringId name_id = 0;
  context.sched_tracker->PushCounter(timestamp, 1000, name_id, cpu,
                                     RefType::kCPU_ID);
  context.sched_tracker->PushCounter(timestamp + 1, 4000, name_id, cpu,
                                     RefType::kCPU_ID);
  context.sched_tracker->PushCounter(timestamp + 3, 5000, name_id, cpu,
                                     RefType::kCPU_ID);
  context.sched_tracker->PushCounter(timestamp + 9, 1000, name_id, cpu,
                                     RefType::kCPU_ID);

  ASSERT_EQ(context.storage->counters().counter_count(), 3ul);
  ASSERT_EQ(context.storage->counters().timestamps().at(0), timestamp);
  ASSERT_EQ(context.storage->counters().durations().at(0), 1);
  ASSERT_EQ(context.storage->counters().values().at(0), 1000);

  ASSERT_EQ(context.storage->counters().timestamps().at(1), timestamp + 1);
  ASSERT_EQ(context.storage->counters().durations().at(1), 2);
  ASSERT_EQ(context.storage->counters().values().at(1), 4000);

  ASSERT_EQ(context.storage->counters().timestamps().at(2), timestamp + 3);
  ASSERT_EQ(context.storage->counters().durations().at(2), 6);
  ASSERT_EQ(context.storage->counters().values().at(2), 5000);
}

TEST_F(SchedTrackerTest, MixedEventsValueDelta) {
  uint32_t cpu = 3;
  uint64_t timestamp = 100;
  StringId name_id_cpu = 0;
  StringId name_id_upid = 0;
  UniquePid upid = 12;
  context.sched_tracker->PushCounter(timestamp, 1000, name_id_cpu, cpu,
                                     RefType::kCPU_ID);
  context.sched_tracker->PushCounter(timestamp + 1, 0, name_id_upid, upid,
                                     RefType::kUTID);
  context.sched_tracker->PushCounter(timestamp + 3, 5000, name_id_cpu, cpu,
                                     RefType::kCPU_ID);
  context.sched_tracker->PushCounter(timestamp + 9, 1, name_id_upid, upid,
                                     RefType::kUTID);

  ASSERT_EQ(context.storage->counters().counter_count(), 2ul);
  ASSERT_EQ(context.storage->counters().timestamps().at(0), timestamp);
  ASSERT_EQ(context.storage->counters().durations().at(0), 3);
  ASSERT_EQ(context.storage->counters().values().at(0), 1000);
  ASSERT_EQ(context.storage->counters().value_deltas().at(0), 4000);

  ASSERT_EQ(context.storage->counters().timestamps().at(1), timestamp + 1);
  ASSERT_EQ(context.storage->counters().durations().at(1), 8);
  ASSERT_EQ(context.storage->counters().values().at(1), 0);
  ASSERT_EQ(context.storage->counters().value_deltas().at(1), 1);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
