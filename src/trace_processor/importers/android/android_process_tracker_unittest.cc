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

#include "src/trace_processor/importers/android/android_process_tracker.h"

#include <memory>
#include <optional>

#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class AndroidProcessTrackerTest : public ::testing::Test {
 public:
  AndroidProcessTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.machine_tracker.reset(new MachineTracker(&context, 0));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.android_process_tracker.reset(new AndroidProcessTracker(&context));
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(AndroidProcessTrackerTest, SameSeqIdReturnsSameProcess) {
  StringId name = context.storage->InternString("app");
  UniquePid upid = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 10, name, ThreadNamePriority::kTrackDescriptor);
  EXPECT_EQ(context.android_process_tracker->GetStartSeqId(upid), 10);

  UniquePid again = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 10, name, ThreadNamePriority::kTrackDescriptor);
  EXPECT_EQ(again, upid);
}

TEST_F(AndroidProcessTrackerTest, NewSeqIdStartsNewProcessWithoutEndingOld) {
  StringId app_a = context.storage->InternString("app_a");
  StringId app_b = context.storage->InternString("app_b");

  UniquePid upid_a = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 1, app_a, ThreadNamePriority::kTrackDescriptor);
  UniquePid upid_b = context.android_process_tracker->GetOrStartProcess(
      5000, 1234, 2, app_b, ThreadNamePriority::kTrackDescriptor);

  EXPECT_NE(upid_a, upid_b);
  EXPECT_EQ(context.process_tracker->GetProcessOrNull(1234), upid_b);
  EXPECT_EQ(context.storage->process_table()[upid_b].start_ts(), 5000);

  // The old process lost the pid, but we do not know when it died so its
  // end_ts stays unset until a death event reports it.
  EXPECT_FALSE(context.storage->process_table()[upid_a].end_ts().has_value());
}

TEST_F(AndroidProcessTrackerTest, FindProcessLocatesRecycledIncarnation) {
  StringId app_a = context.storage->InternString("app_a");
  StringId app_b = context.storage->InternString("app_b");

  UniquePid upid_a = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 1, app_a, ThreadNamePriority::kTrackDescriptor);
  UniquePid upid_b = context.android_process_tracker->GetOrStartProcess(
      5000, 1234, 2, app_b, ThreadNamePriority::kTrackDescriptor);

  // Both incarnations remain findable by their seq id, including the one that
  // no longer owns the pid. This is what a late death event relies on.
  EXPECT_EQ(context.android_process_tracker->FindProcess(1234, 1), upid_a);
  EXPECT_EQ(context.android_process_tracker->FindProcess(1234, 2), upid_b);
  EXPECT_EQ(context.android_process_tracker->FindProcess(1234, 3),
            std::nullopt);
}

TEST_F(AndroidProcessTrackerTest, AdoptsLiveProcessWithNoSeqId) {
  StringId name = context.storage->InternString("app");
  UniquePid existing = context.process_tracker->GetOrCreateProcess(1234);

  UniquePid upid = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 7, name, ThreadNamePriority::kTrackDescriptor);

  // Nothing told us the pid was recycled, so we keep the existing process and
  // stamp the seq id onto it rather than splitting.
  EXPECT_EQ(upid, existing);
  EXPECT_EQ(context.android_process_tracker->GetStartSeqId(upid), 7);
}

TEST_F(AndroidProcessTrackerTest, MissingSeqIdKeepsLiveProcess) {
  StringId name = context.storage->InternString("app");
  UniquePid upid = context.android_process_tracker->GetOrStartProcess(
      1000, 1234, 5, name, ThreadNamePriority::kTrackDescriptor);

  // A record with no seq id cannot be told apart from seq id 0, so it must not
  // be treated as a new incarnation.
  UniquePid same = context.android_process_tracker->GetOrStartProcess(
      2000, 1234, std::nullopt, name, ThreadNamePriority::kTrackDescriptor);
  EXPECT_EQ(same, upid);
  EXPECT_EQ(context.android_process_tracker->GetStartSeqId(upid), 5);
}

}  // namespace
}  // namespace perfetto::trace_processor
