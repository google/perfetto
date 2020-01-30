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

#include "perfetto/base/logging.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/track_tracker.h"
#include "test/gtest_and_gmock.h"

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
    context.global_args_tracker.reset(new GlobalArgsTracker(&context));
    context.args_tracker.reset(new ArgsTracker(&context));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.event_tracker.reset(new EventTracker(&context));
    context.track_tracker.reset(new TrackTracker(&context));
    sched_tracker = SchedEventTracker::GetOrCreate(&context);
  }

 protected:
  TraceProcessorContext context;
  SchedEventTracker* sched_tracker;
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

  sched_tracker->PushSchedSwitch(cpu, timestamp, pid_1, kCommProc2, prio,
                                 prev_state, pid_2, kCommProc1, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 1ul);

  sched_tracker->PushSchedSwitch(cpu, timestamp + 1, pid_2, kCommProc1, prio,
                                 prev_state, pid_1, kCommProc2, prio);

  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 2ul);

  const auto& timestamps = context.storage->sched_slice_table().ts();
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->thread_table().start_ts()[1], base::nullopt);

  auto name =
      context.storage->GetString(context.storage->thread_table().name()[1]);
  ASSERT_STREQ(name.c_str(), kCommProc1);
  ASSERT_EQ(context.storage->sched_slice_table().utid()[0], 1u);
  ASSERT_EQ(context.storage->sched_slice_table().dur()[0], 1);
}

TEST_F(EventTrackerTest, InsertThirdSched_SameThread) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  int64_t prev_state = 32;
  static const char kCommProc1[] = "process1";
  static const char kCommProc2[] = "process2";
  int32_t prio = 1024;

  sched_tracker->PushSchedSwitch(cpu, timestamp, /*tid=*/4, kCommProc2, prio,
                                 prev_state,
                                 /*tid=*/2, kCommProc1, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 1u);

  sched_tracker->PushSchedSwitch(cpu, timestamp + 1, /*tid=*/2, kCommProc1,
                                 prio, prev_state,
                                 /*tid=*/4, kCommProc2, prio);
  sched_tracker->PushSchedSwitch(cpu, timestamp + 11, /*tid=*/4, kCommProc2,
                                 prio, prev_state,
                                 /*tid=*/2, kCommProc1, prio);
  sched_tracker->PushSchedSwitch(cpu, timestamp + 31, /*tid=*/2, kCommProc1,
                                 prio, prev_state,
                                 /*tid=*/4, kCommProc2, prio);
  ASSERT_EQ(context.storage->sched_slice_table().row_count(), 4ul);

  const auto& timestamps = context.storage->sched_slice_table().ts();
  ASSERT_EQ(timestamps[0], timestamp);
  ASSERT_EQ(context.storage->thread_table().start_ts()[1], base::nullopt);
  ASSERT_EQ(context.storage->sched_slice_table().dur()[0], 1u);
  ASSERT_EQ(context.storage->sched_slice_table().dur()[1], 11u - 1u);
  ASSERT_EQ(context.storage->sched_slice_table().dur()[2], 31u - 11u);
  ASSERT_EQ(context.storage->sched_slice_table().utid()[0],
            context.storage->sched_slice_table().utid()[2]);
}

TEST_F(EventTrackerTest, CounterDuration) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;
  StringId name_id = kNullStringId;

  TrackId track = context.track_tracker->InternCpuCounterTrack(name_id, cpu);
  context.event_tracker->PushCounter(timestamp, 1000, track);
  context.event_tracker->PushCounter(timestamp + 1, 4000, track);
  context.event_tracker->PushCounter(timestamp + 3, 5000, track);
  context.event_tracker->PushCounter(timestamp + 9, 1000, track);

  ASSERT_EQ(context.storage->counter_track_table().row_count(), 1ul);

  ASSERT_EQ(context.storage->counter_table().row_count(), 4ul);
  ASSERT_EQ(context.storage->counter_table().ts()[0], timestamp);
  ASSERT_DOUBLE_EQ(context.storage->counter_table().value()[0], 1000);

  ASSERT_EQ(context.storage->counter_table().ts()[1], timestamp + 1);
  ASSERT_DOUBLE_EQ(context.storage->counter_table().value()[1], 4000);

  ASSERT_EQ(context.storage->counter_table().ts()[2], timestamp + 3);
  ASSERT_DOUBLE_EQ(context.storage->counter_table().value()[2], 5000);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
