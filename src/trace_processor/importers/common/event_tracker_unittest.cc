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

#include "src/trace_processor/importers/common/event_tracker.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
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
    context.global_args_tracker.reset(
        new GlobalArgsTracker(context.storage.get()));
    context.args_tracker.reset(new ArgsTracker(&context));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.event_tracker.reset(new EventTracker(&context));
    context.track_tracker.reset(new TrackTracker(&context));
    context.machine_tracker.reset(new MachineTracker(&context, 0));
    context.cpu_tracker.reset(new CpuTracker(&context));
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(EventTrackerTest, CounterDuration) {
  uint32_t cpu = 3;
  int64_t timestamp = 100;

  TrackId track =
      context.track_tracker->InternCpuCounterTrack(tracks::cpu_frequency, cpu);
  context.event_tracker->PushCounter(timestamp, 1000, track);
  context.event_tracker->PushCounter(timestamp + 1, 4000, track);
  context.event_tracker->PushCounter(timestamp + 3, 5000, track);
  context.event_tracker->PushCounter(timestamp + 9, 1000, track);

  ASSERT_EQ(context.storage->counter_track_table().row_count(), 1ul);

  const auto& counter = context.storage->counter_table();
  ASSERT_EQ(counter.row_count(), 4ul);

  auto rr = counter[0];
  ASSERT_EQ(rr.ts(), timestamp);
  ASSERT_DOUBLE_EQ(rr.value(), 1000);

  rr = counter[1];
  ASSERT_EQ(rr.ts(), timestamp + 1);
  ASSERT_DOUBLE_EQ(rr.value(), 4000);

  rr = counter[2];
  ASSERT_EQ(rr.ts(), timestamp + 3);
  ASSERT_DOUBLE_EQ(rr.value(), 5000);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
