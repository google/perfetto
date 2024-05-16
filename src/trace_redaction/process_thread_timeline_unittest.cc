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
constexpr uint64_t kTimeH = 70;

constexpr int32_t kPidA = 1;
constexpr int32_t kPidB = 2;
constexpr int32_t kPidC = 3;
constexpr int32_t kPidD = 4;

constexpr uint64_t kUidA = 97;
constexpr uint64_t kUidC = 99;

}  // namespace

// B        C        D   E   F        G        H
// *        *        *   *   *        *        *
// |----- PID B -----|   .   |----- PID B -----|
//          |--------- PID C ---------|
//          | <- PID D (no duration)
class ProcessThreadTimelineTest : public testing::Test {
 protected:
  void SetUp() {
    for (auto e : pid_b_events_) {
      timeline_.Append(e);
    }

    for (auto e : pid_c_events_) {
      timeline_.Append(e);
    }

    for (auto e : pid_d_events_) {
      timeline_.Append(e);
    }

    timeline_.Sort();
  }

  ProcessThreadTimeline::Event invalid_ = {};

  std::array<ProcessThreadTimeline::Event, 4> pid_b_events_ = {
      ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA, kUidA),
      ProcessThreadTimeline::Event::Close(kTimeD, kPidB),
      ProcessThreadTimeline::Event::Open(kTimeF, kPidB, kPidA, kUidA),
      ProcessThreadTimeline::Event::Close(kTimeH, kPidB),
  };

  std::array<ProcessThreadTimeline::Event, 2> pid_c_events_ = {
      ProcessThreadTimeline::Event::Open(kTimeC, kPidC, kPidA, kUidA),
      ProcessThreadTimeline::Event::Close(kTimeG, kPidC),
  };

  // A process with no duration.
  std::array<ProcessThreadTimeline::Event, 2> pid_d_events_{
      ProcessThreadTimeline::Event::Open(kTimeC, kPidD, kPidA, kUidA),
      ProcessThreadTimeline::Event::Close(kTimeC, kPidD),
  };

  ProcessThreadTimeline timeline_;
};

TEST_F(ProcessThreadTimelineTest, NoEventBeforeFirstSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeA, kPidB);
  ASSERT_EQ(event, invalid_);
}

TEST_F(ProcessThreadTimelineTest, OpenEventAtStartOfFirstSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeB, kPidB);
  ASSERT_EQ(event, pid_b_events_[0]);
}

TEST_F(ProcessThreadTimelineTest, OpenEventWithinFirstSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeC, kPidB);
  ASSERT_EQ(event, pid_b_events_[0]);
}

TEST_F(ProcessThreadTimelineTest, CloseEventAtEndOfFirstSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeD, kPidB);
  ASSERT_EQ(event, pid_b_events_[1]);
}

TEST_F(ProcessThreadTimelineTest, CloseEventBetweenSpans) {
  auto event = timeline_.FindPreviousEvent(kTimeE, kPidB);
  ASSERT_EQ(event, pid_b_events_[1]);
}

TEST_F(ProcessThreadTimelineTest, OpenEventAtStartOfSecondSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeF, kPidB);
  ASSERT_EQ(event, pid_b_events_[2]);
}

TEST_F(ProcessThreadTimelineTest, OpenEventWithinSecondSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeG, kPidB);
  ASSERT_EQ(event, pid_b_events_[2]);
}

TEST_F(ProcessThreadTimelineTest, CloseEventAtEndOfSecondSpan) {
  auto event = timeline_.FindPreviousEvent(kTimeH, kPidB);
  ASSERT_EQ(event, pid_b_events_[3]);
}

// Pid B is active. But Pid C is not active. At this point, Pid C should report
// as invalid event though another pid is active.
TEST_F(ProcessThreadTimelineTest, InvalidEventWhenAnotherSpanIsActive) {
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeB, kPidB), pid_b_events_[0]);
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeB, kPidC), invalid_);
}

// When both pids are active, they should both report as active (using their
// open events).
TEST_F(ProcessThreadTimelineTest, ConcurrentSpansBothReportAsActive) {
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeC, kPidB), pid_b_events_[0]);
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeC, kPidC), pid_c_events_[0]);
}

// There are three test cases here:
//
// 1. Before open/close
// 2. At open/close
// 3. After open/close
//
// Normally these would be tree different test cases, but the naming gets
// complicated, so it is easier to do it in one case.
TEST_F(ProcessThreadTimelineTest, ZeroDuration) {
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeB, kPidD), invalid_);
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeC, kPidD), pid_d_events_[1]);
  ASSERT_EQ(timeline_.FindPreviousEvent(kTimeD, kPidD), pid_d_events_[1]);
}

// |----- UID A -----| |----- UID C -----|
//  |---- PID A ----|   |---- PID C ----|
//    |-- PID B --|
//
// NOTE: The notation above does not represent time, it represent relationship.
// For example, PID B is a child of PID A.
class ProcessThreadTimelineIsConnectedTest : public testing::Test {
 protected:
  void SetUp() {
    timeline_.Append(ProcessThreadTimeline::Event::Open(
        kTimeB, kPidA, ProcessThreadTimeline::Event::kUnknownPid, kUidA));
    timeline_.Append(ProcessThreadTimeline::Event::Open(kTimeB, kPidB, kPidA));
    timeline_.Append(ProcessThreadTimeline::Event::Open(
        kTimeB, kPidC, ProcessThreadTimeline::Event::kUnknownPid, kUidC));
    timeline_.Sort();
  }

  ProcessThreadTimeline timeline_;
};

// PID A is directly connected to UID A.
TEST_F(ProcessThreadTimelineIsConnectedTest, DirectPidAndUid) {
  ASSERT_TRUE(timeline_.PidConnectsToUid(kTimeB, kPidA, kUidA));
}

// PID B is indirectly connected to UID A through PID A.
TEST_F(ProcessThreadTimelineIsConnectedTest, IndirectPidAndUid) {
  ASSERT_TRUE(timeline_.PidConnectsToUid(kTimeB, kPidB, kUidA));
}

// UID A and UID C are valid packages. However, PID B is connected to UID A, not
// UID C.
TEST_F(ProcessThreadTimelineIsConnectedTest, NotConnectedToOtherUid) {
  ASSERT_FALSE(timeline_.PidConnectsToUid(kTimeB, kPidB, kUidC));
}

// PID D is not in the timeline, so it shouldn't be connected to anything.
TEST_F(ProcessThreadTimelineIsConnectedTest, MissingPid) {
  ASSERT_FALSE(timeline_.PidConnectsToUid(kTimeB, kPidD, kUidA));
}

// Even through there is a connection between PID A and UID A, the query is too
// soon (events are at TIME B, but the query is at TIME A).
TEST_F(ProcessThreadTimelineIsConnectedTest, PrematureDirectPidAndUid) {
  ASSERT_FALSE(timeline_.PidConnectsToUid(kTimeA, kPidA, kUidA));
}

}  // namespace perfetto::trace_redaction
