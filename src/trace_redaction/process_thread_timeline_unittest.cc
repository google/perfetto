/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <cstdint>

#include "src/trace_redaction/process_thread_timeline.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

namespace {

class SliceTestParams {
 public:
  SliceTestParams(uint64_t ts, int32_t pid, uint64_t uid)
      : ts_(ts), pid_(pid), uid_(uid) {}

  uint64_t ts() const { return ts_; }
  int32_t pid() const { return pid_; }
  uint64_t uid() const { return uid_; }

 private:
  uint64_t ts_;
  int32_t pid_;
  uint64_t uid_;
};

constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = 10;
constexpr uint64_t kTimeC = 20;
constexpr uint64_t kTimeD = 30;
constexpr uint64_t kTimeE = 40;
constexpr uint64_t kTimeF = 50;
constexpr uint64_t kTimeG = 60;

constexpr int32_t kPidA = 1;
constexpr int32_t kPidB = 2;

constexpr uint64_t kUidA = 98;
constexpr uint64_t kUidB = 99;

}  // namespace

// |--- PID A --- >
class TimelineEventsOpenTest : public testing::Test {
 protected:
  void SetUp() {
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

TEST_F(TimelineEventsOpenTest, ReturnsNothingBeforeStart) {
  auto slice = timeline_.Search(kTimeA, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

TEST_F(TimelineEventsOpenTest, ReturnsSomethingAtStart) {
  auto slice = timeline_.Search(kTimeB, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidA);
}

TEST_F(TimelineEventsOpenTest, ReturnsSomethingAfterStart) {
  auto slice = timeline_.Search(kTimeC, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidA);
}

// |--- PID A --- |
class TimelineEventsCloseTest : public testing::Test {
 protected:
  void SetUp() {
    // An open event must exist in order for a close event to exist.
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA));
    timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeD, kPidB));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

TEST_F(TimelineEventsCloseTest, ReturnsSomethingBeforeClose) {
  auto slice = timeline_.Search(kTimeC, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidA);
}

TEST_F(TimelineEventsCloseTest, ReturnsNothingAtClose) {
  auto slice = timeline_.Search(kTimeD, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

TEST_F(TimelineEventsCloseTest, ReturnsNothingAfterClose) {
  auto slice = timeline_.Search(kTimeE, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

// Two start events can occur (normally with process trees). The timeline is
// expected to treat this case as if there was a close event between the two
// open events.
//
// |--- PID A --- >
//                 |--- PID A --- >
class TimelineEventsOpenAfterOpenTest : public testing::Test {
 protected:
  void SetUp() {
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA));
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeD, kPidB, kPidA, kUidB));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

TEST_F(TimelineEventsOpenAfterOpenTest, ReturnsFirstBeforeSwitch) {
  auto slice = timeline_.Search(kTimeC, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidA);
}

TEST_F(TimelineEventsOpenAfterOpenTest, ReturnsSecondAtSwitch) {
  auto slice = timeline_.Search(kTimeD, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidB);
}

TEST_F(TimelineEventsOpenAfterOpenTest, ReturnsSecondAfterSwitch) {
  auto slice = timeline_.Search(kTimeE, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidB);
}

// |----- PID_A -----|
//          |----- PID_B -----|
class TimelineEventsOverlappingRangesTest : public testing::Test {
 protected:
  void SetUp() {
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, 0, kUidA));
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeC, kPidB, 0, kUidB));
    timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeE, kPidA));
    timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeG, kPidB));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

TEST_F(TimelineEventsOverlappingRangesTest, FindProcessADuringOverlap) {
  auto slice = timeline_.Search(kTimeD, kPidA);
  ASSERT_EQ(slice.pid, kPidA);
  ASSERT_EQ(slice.uid, kUidA);
}

TEST_F(TimelineEventsOverlappingRangesTest, FindProcessBDuringOverlap) {
  auto slice = timeline_.Search(kTimeD, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidB);
}

// |------------- PID_A ------------->
//         |----- PID_B -----|
class TimelineEventsParentChildTest : public testing::Test {
 protected:
  void SetUp() {
    // PID A's parent (0) does not exist on the timeline. In production, this is
    // what happens as the root process (0) doesn't exist.
    timeline_.Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, 0, kUidA));
    timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeC, kPidB, kPidA));
    timeline_.Append(ProcessThreadTimeline::Event::Close(kTimeE, kPidB));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

TEST_F(TimelineEventsParentChildTest, InvalidBeforeBStarts) {
  auto slice = timeline_.Search(kTimeB, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

TEST_F(TimelineEventsParentChildTest, ValidAfterBStarts) {
  auto slice = timeline_.Search(kTimeD, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, kUidA);
}

TEST_F(TimelineEventsParentChildTest, InvalidAfterBEnds) {
  auto slice = timeline_.Search(kTimeF, kPidB);
  ASSERT_EQ(slice.pid, kPidB);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

}  // namespace perfetto::trace_redaction
