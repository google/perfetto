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

#include <array>
#include <cstdint>

#include "src/trace_redaction/process_thread_timeline.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

namespace {

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

TEST_F(ProcessThreadTimelineTest, BeforeSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeA, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_FALSE(prev_open);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeA, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_FALSE(prev_close);
}

TEST_F(ProcessThreadTimelineTest, StartOfSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeB, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[0]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeB, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_FALSE(prev_close);
}

TEST_F(ProcessThreadTimelineTest, DuringSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeC, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[0]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeC, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_FALSE(prev_close);
}

TEST_F(ProcessThreadTimelineTest, EndOfSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeD, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[0]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeD, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
  ASSERT_EQ(*prev_close, pid_b_events_[1]);
}

// Even through its after a span, the previous open and close events should be
// openned.
TEST_F(ProcessThreadTimelineTest, AfterSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeE, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[0]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeE, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
  ASSERT_EQ(*prev_close, pid_b_events_[1]);
}

// When a pid is reused, the new open event (for the reused pid) should be
// returned, but the close from the previous span should be returned.
TEST_F(ProcessThreadTimelineTest, StartOfSecondSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeF, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[2]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeF, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
  ASSERT_EQ(*prev_close, pid_b_events_[1]);
}

// Now that there is a second close event, both open and close events should
// come from the same span.
TEST_F(ProcessThreadTimelineTest, CloseOfSecondSpan) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeH, kPidB, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_b_events_[2]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeH, kPidB, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
  ASSERT_EQ(*prev_close, pid_b_events_[3]);
}

TEST_F(ProcessThreadTimelineTest, BeforeSpanWithZeroDuration) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeA, kPidD, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_FALSE(prev_open);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeA, kPidD, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_FALSE(prev_close);
}

TEST_F(ProcessThreadTimelineTest, SpanWithZeroDuration) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeC, kPidD, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);
  ASSERT_EQ(*prev_open, pid_d_events_[0]);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeC, kPidD, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
  ASSERT_EQ(*prev_close, pid_d_events_[1]);
}

TEST_F(ProcessThreadTimelineTest, AfterSpanWithZeroDuration) {
  auto prev_open = timeline_.QueryLeftMax(
      kTimeE, kPidD, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_TRUE(prev_open);

  auto prev_close = timeline_.QueryLeftMax(
      kTimeE, kPidD, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_TRUE(prev_close);
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
