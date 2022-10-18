/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/active_chrome_processes_tracker.h"

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

static bool operator==(const ProcessWithDataLoss& lhs,
                       const ProcessWithDataLoss& rhs) {
  return lhs.upid == rhs.upid && lhs.reliable_from == rhs.reliable_from;
}

namespace {

using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

constexpr int64_t kNanosecondsInSecond = 1000 * 1000 * 1000;

TEST(ActiveChromeProcessesTrackerTest, NoMetadataAndNoDescriptors) {
  // No metadata and no descriptor = no processes are missing.
  ActiveChromeProcessesTracker tracker(nullptr);
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(), IsEmpty());
}

TEST(ActiveChromeProcessesTrackerTest, NoDescriptors) {
  ActiveChromeProcessesTracker tracker(nullptr);
  tracker.AddActiveProcessMetadata(/*timestamp=*/10, /*upid=*/1);
  tracker.AddActiveProcessMetadata(/*timestamp=*/10, /*upid=*/2);
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(),
              UnorderedElementsAre(ProcessWithDataLoss{1, base::nullopt},
                                   ProcessWithDataLoss{2, base::nullopt}));
}

TEST(ActiveChromeProcessesTrackerTest, InexactMatch) {
  ActiveChromeProcessesTracker tracker(nullptr);
  tracker.AddActiveProcessMetadata(/*timestamp=*/10 * kNanosecondsInSecond,
                                   /*upid=*/1);
  tracker.AddActiveProcessMetadata(/*timestamp=*/15 * kNanosecondsInSecond,
                                   /*upid=*/1);
  tracker.AddProcessDescriptor(
      /*timestamp=*/10 * kNanosecondsInSecond - 200 * 1000 * 1000, /*upid=*/1);
  tracker.AddProcessDescriptor(
      /*timestamp=*/15 * kNanosecondsInSecond + 200 * 1000 * 1000, /*upid=*/1);
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(), IsEmpty());
}

TEST(ActiveChromeProcessesTrackerTest, InexactMatchTooBigDiff) {
  ActiveChromeProcessesTracker tracker(nullptr);
  tracker.AddActiveProcessMetadata(/*timestamp=*/10 * kNanosecondsInSecond,
                                   /*upid=*/1);
  tracker.AddActiveProcessMetadata(/*timestamp=*/15 * kNanosecondsInSecond,
                                   /*upid=*/1);
  tracker.AddProcessDescriptor(
      /*timestamp=*/10 * kNanosecondsInSecond - 200 * 1000 * 1000 - 1,
      /*upid=*/1);
  tracker.AddProcessDescriptor(
      /*timestamp=*/15 * kNanosecondsInSecond + 200 * 1000 * 1000 + 1,
      /*upid=*/1);
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(),
              UnorderedElementsAre(ProcessWithDataLoss{
                  1, 15 * kNanosecondsInSecond + 200 * 1000 * 1000 + 1}));
}

TEST(ActiveChromeProcessesTrackerTest, ExtraDescriptor) {
  // There're more descriptors than metadata packets - this is OK.
  ActiveChromeProcessesTracker tracker(nullptr);
  tracker.AddActiveProcessMetadata(/*timestamp=*/15 * kNanosecondsInSecond,
                                   /*upid=*/1);
  tracker.AddProcessDescriptor(/*timestamp=*/10 * kNanosecondsInSecond,
                               /*upid=*/1);
  tracker.AddProcessDescriptor(/*timestamp=*/15 * kNanosecondsInSecond,
                               /*upid=*/1);
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(), IsEmpty());
}

TEST(ActiveChromeProcessesTracker, TemrinatedProcess) {
  ActiveChromeProcessesTracker tracker(nullptr);
  // First metadata packet - two processes.
  tracker.AddActiveProcessMetadata(/*timestamp=*/10, /*upid=*/1);
  tracker.AddActiveProcessMetadata(/*timestamp=*/10, /*upid=*/2);
  // Second metadata packet - only one process, the first process terminated.
  tracker.AddActiveProcessMetadata(/*timestamp=*/15, /*upid=*/2);

  // The first process is reliable since the second snapshot - it terminated,
  // so it has no data loss.
  // The second process has data loss till the end of the trace.
  EXPECT_THAT(tracker.GetProcessesWithDataLoss(),
              UnorderedElementsAre(ProcessWithDataLoss{1, 15},
                                   ProcessWithDataLoss{2, base::nullopt}));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
