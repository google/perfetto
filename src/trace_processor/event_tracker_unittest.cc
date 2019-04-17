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

#include "src/trace_processor/event_tracker.h"

#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/process_tracker.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class EventTrackerTest : public ::testing::Test {
 public:
  EventTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.args_tracker.reset(new ArgsTracker(&context));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.event_tracker.reset(new EventTracker(&context));
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(EventTrackerTest, InsertSecondSched) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  uint32_t pid_1 = 2;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  uint32_t pid_2 = 4;
  int32_t prio = 1024;

  const auto& timestamps = context.storage->slices().start_ns();
  context.event_tracker->PushSchedSwitch(cpu, timestamp, pid_1, kCommProc2,
                                         prio, prev_state, pid_2, kCommProc1,
                                         prio);
  ASSERT_EQ(timestamps.size(), 1);

  context.event_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, kCommProc1,
                                         prio, prev_state, pid_1, kCommProc2,
                                         prio);

  ASSERT_EQ(timestamps.size(), 2ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, 0);
  ASSERT_STREQ(
      context.storage->GetString(context.storage->GetThread(1).name_id).c_str(),
      kCommProc1);
  ASSERT_EQ(context.storage->slices().utids().front(), 1);
  ASSERT_EQ(context.storage->slices().durations().front(), 1);
}

TEST_F(EventTrackerTest, InsertThirdSched_SameThread) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  int32_t prio = 1024;

  const auto& timestamps = context.storage->slices().start_ns();
  context.event_tracker->PushSchedSwitch(cpu, timestamp, /*tid=*/4, kCommProc2,
                                         prio, prev_state,
                                         /*tid=*/2, kCommProc1, prio);
  ASSERT_EQ(timestamps.size(), 1);

  context.event_tracker->PushSchedSwitch(cpu, timestamp + 1, /*tid=*/2,
                                         kCommProc1, prio, prev_state,
                                         /*tid=*/4, kCommProc2, prio);
  context.event_tracker->PushSchedSwitch(cpu, timestamp + 11, /*tid=*/4,
                                         kCommProc2, prio, prev_state,
                                         /*tid=*/2, kCommProc1, prio);
  context.event_tracker->PushSchedSwitch(cpu, timestamp + 31, /*tid=*/2,
                                         kCommProc1, prio, prev_state,
                                         /*tid=*/4, kCommProc2, prio);

  ASSERT_EQ(timestamps.size(), 4ul);
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->GetThread(1).start_ns, 0);
  ASSERT_EQ(context.storage->slices().durations().at(0), 1u);
  ASSERT_EQ(context.storage->slices().durations().at(1), 11u - 1u);
  ASSERT_EQ(context.storage->slices().durations().at(2), 31u - 11u);
  ASSERT_EQ(context.storage->slices().utids().at(0),
            context.storage->slices().utids().at(2));
}

TEST_F(EventTrackerTest, CounterDuration) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  StringId name_id = 0;
  context.event_tracker->PushCounter(timestamp, 1000, name_id, cpu,
                                     RefType::kRefCpuId);
  context.event_tracker->PushCounter(timestamp + 1, 4000, name_id, cpu,
                                     RefType::kRefCpuId);
  context.event_tracker->PushCounter(timestamp + 3, 5000, name_id, cpu,
                                     RefType::kRefCpuId);
  context.event_tracker->PushCounter(timestamp + 9, 1000, name_id, cpu,
                                     RefType::kRefCpuId);

  ASSERT_EQ(context.storage->counter_definitions().size(), 1ul);

  ASSERT_EQ(context.storage->counter_values().size(), 4ul);
  ASSERT_EQ(context.storage->counter_values().timestamps().at(0), timestamp);
  ASSERT_EQ(context.storage->counter_values().values().at(0), 1000);

  ASSERT_EQ(context.storage->counter_values().timestamps().at(1),
            timestamp + 1);
  ASSERT_EQ(context.storage->counter_values().values().at(1), 4000);

  ASSERT_EQ(context.storage->counter_values().timestamps().at(2),
            timestamp + 3);
  ASSERT_EQ(context.storage->counter_values().values().at(2), 5000);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
