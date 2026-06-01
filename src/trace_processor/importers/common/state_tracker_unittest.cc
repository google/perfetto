/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/importers/common/state_tracker.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class StateTrackerTest : public ::testing::Test {
 protected:
  StateTrackerTest() {
    context_.storage.reset(new TraceStorage());
    context_.state_tracker =
        TraceProcessorContextPtr<StateTracker>::MakeRoot(&context_);
  }

  TraceProcessorContext context_;
};

TEST_F(StateTrackerTest, UpdateState) {
  auto* tracker = context_.state_tracker.get();

  constexpr TrackId track{22u};
  const StringId state1 = context_.storage->InternString("state1");
  const StringId state2 = context_.storage->InternString("state2");
  const StringId cat1 = context_.storage->InternString("cat1");
  const StringId cat2 = context_.storage->InternString("cat2");

  // 1. Start state1
  tracker->UpdateState(2 /*ts*/, track, state1, cat1);

  // 2. Update with same state
  tracker->UpdateState(5 /*ts*/, track, state1, cat1);

  // 3. Update with different state (should end state1 and start state2)
  tracker->UpdateState(10 /*ts*/, track, state2, cat2);

  // 4. Update with empty state (should end state2)
  tracker->UpdateState(15 /*ts*/, track, kNullStringId);

  const auto& states = context_.storage->state_table();
  EXPECT_EQ(states.row_count(), 2u);

  // First state (state1) should be closed at ts=10
  auto sr0 = states[0];
  EXPECT_EQ(sr0.ts(), 2);
  EXPECT_EQ(sr0.dur(), 8);  // 10 - 2
  EXPECT_EQ(sr0.value().raw_id(), state1.raw_id());
  EXPECT_EQ(sr0.category().value_or(kNullStringId).raw_id(), cat1.raw_id());

  // Second state (state2) should start at ts=10 and be closed at ts=15
  auto sr1 = states[1];
  EXPECT_EQ(sr1.ts(), 10);
  EXPECT_EQ(sr1.dur(), 5);  // 15 - 10
  EXPECT_EQ(sr1.value().raw_id(), state2.raw_id());
  EXPECT_EQ(sr1.category().value_or(kNullStringId).raw_id(), cat2.raw_id());
}

}  // namespace
}  // namespace perfetto::trace_processor
