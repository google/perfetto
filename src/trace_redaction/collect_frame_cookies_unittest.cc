
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

#include "src/trace_redaction/collect_frame_cookies.h"
#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/frame_timeline_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

constexpr uint64_t kTimeStep = 1000;

constexpr uint64_t kTimestampA = 0;
constexpr uint64_t kTimestampB = kTimeStep;
constexpr uint64_t kTimestampC = kTimeStep * 2;
constexpr uint64_t kTimestampD = kTimeStep * 3;
constexpr uint64_t kTimestampE = kTimeStep * 4;

constexpr int64_t kCookieA = 1234;
constexpr int64_t kCookieB = 2345;

// Start at 1, amd not zero, because zero hnas special meaning (system uid).
constexpr uint64_t kUidA = 1;

constexpr int32_t kPidNone = 10;
constexpr int32_t kPidA = 11;

enum class FrameCookieType {
  ExpectedSurface,
  ExpectedDisplay,
  ActualSurface,
  ActualDisplay,
};

protos::gen::TracePacket CreateExpectedSurfaceFrameStart(uint64_t ts,
                                                         int32_t pid,
                                                         int64_t cookie) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(ts);
  auto* start = packet.mutable_frame_timeline_event()
                    ->mutable_expected_surface_frame_start();
  start->set_cookie(cookie);
  start->set_pid(pid);
  return packet;
}

protos::gen::TracePacket CreateActualSurfaceFrameStart(uint64_t ts,
                                                       int32_t pid,
                                                       int64_t cookie) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(ts);
  auto* start = packet.mutable_frame_timeline_event()
                    ->mutable_actual_surface_frame_start();
  start->set_cookie(cookie);
  start->set_pid(pid);

  return packet;
}

protos::gen::TracePacket CreateExpectedDisplayFrameStart(uint64_t ts,
                                                         int32_t pid,
                                                         int64_t cookie) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(ts);
  auto* start = packet.mutable_frame_timeline_event()
                    ->mutable_expected_display_frame_start();
  start->set_cookie(cookie);
  start->set_pid(pid);

  return packet;
}

protos::gen::TracePacket CreateActualDisplayFrameStart(uint64_t ts,
                                                       int32_t pid,
                                                       int64_t cookie) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(ts);
  auto* start = packet.mutable_frame_timeline_event()
                    ->mutable_actual_display_frame_start();
  start->set_cookie(cookie);
  start->set_pid(pid);

  return packet;
}

protos::gen::TracePacket CreateFrameEnd(uint64_t ts, int64_t cookie) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(ts);

  auto* start = packet.mutable_frame_timeline_event()->mutable_frame_end();
  start->set_cookie(cookie);

  return packet;
}

protos::gen::TracePacket CreateStartPacket(FrameCookieType type,
                                           uint64_t ts,
                                           int32_t pid,
                                           int64_t cookie) {
  switch (type) {
    case FrameCookieType::ExpectedSurface:
      return CreateExpectedSurfaceFrameStart(ts, pid, cookie);
    case FrameCookieType::ExpectedDisplay:
      return CreateExpectedDisplayFrameStart(ts, pid, cookie);
    case FrameCookieType::ActualSurface:
      return CreateActualSurfaceFrameStart(ts, pid, cookie);
    case FrameCookieType::ActualDisplay:
      return CreateActualDisplayFrameStart(ts, pid, cookie);
  }

  PERFETTO_FATAL("Unhandled case. This should never happen.");
}

void CollectCookies(const std::string& packet, Context* context) {
  protos::pbzero::TracePacket::Decoder decoder(packet);

  CollectFrameCookies collect_;
  ASSERT_OK(collect_.Begin(context));
  ASSERT_OK(collect_.Collect(decoder, context));
  ASSERT_OK(collect_.End(context));
}

class FrameCookieTest : public testing::Test {
 protected:
  CollectFrameCookies collect_;
  Context context_;
};

TEST_F(FrameCookieTest, ExtractsExpectedSurfaceFrameStart) {
  auto packet = CreateExpectedSurfaceFrameStart(kTimestampA, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_EQ(context_.global_frame_cookies.size(), 1u);

  const auto& cookie = context_.global_frame_cookies.back();
  ASSERT_EQ(cookie.cookie, kCookieA);
  ASSERT_EQ(cookie.pid, kPidA);
  ASSERT_EQ(cookie.ts, kTimestampA);
}

TEST_F(FrameCookieTest, ExtractsActualSurfaceFrameStart) {
  auto packet = CreateActualSurfaceFrameStart(kTimestampA, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_EQ(context_.global_frame_cookies.size(), 1u);

  const auto& cookie = context_.global_frame_cookies.back();
  ASSERT_EQ(cookie.cookie, kCookieA);
  ASSERT_EQ(cookie.pid, kPidA);
  ASSERT_EQ(cookie.ts, kTimestampA);
}

TEST_F(FrameCookieTest, ExtractsExpectedDisplayFrameStart) {
  auto packet = CreateExpectedDisplayFrameStart(kTimestampA, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_EQ(context_.global_frame_cookies.size(), 1u);

  const auto& cookie = context_.global_frame_cookies.back();
  ASSERT_EQ(cookie.cookie, kCookieA);
  ASSERT_EQ(cookie.pid, kPidA);
  ASSERT_EQ(cookie.ts, kTimestampA);
}

TEST_F(FrameCookieTest, ExtractsActualDisplayFrameStart) {
  auto packet = CreateActualDisplayFrameStart(kTimestampA, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_EQ(context_.global_frame_cookies.size(), 1u);

  const auto& cookie = context_.global_frame_cookies.back();
  ASSERT_EQ(cookie.cookie, kCookieA);
  ASSERT_EQ(cookie.pid, kPidA);
  ASSERT_EQ(cookie.ts, kTimestampA);
}

// End events have no influence during the collect phase because they don't have
// a direct connection to a process. They're indirectly connected to a pid via a
// start event (via a common cookie value).
TEST_F(FrameCookieTest, IgnoresFrameEnd) {
  auto packet = CreateFrameEnd(kTimestampA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_TRUE(context_.global_frame_cookies.empty());
}

class ReduceFrameCookiesTest
    : public testing::Test,
      public testing::WithParamInterface<FrameCookieType> {
 protected:
  void SetUp() {
    context_.package_uid = kUidA;

    // Time A   +- Time B       +- Time C    +- Time D   +- Time E
    //          |                            |
    //          +------------ Pid A ---------+
    //
    // The pid will be active from time b to time d. Time A will be used for
    // "before active". Time C will be used for "while active". Time E will be
    // used for "after active".
    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        kTimestampB, kPidA, kPidNone, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Close(kTimestampD, kPidA));
    context_.timeline->Sort();
  }

  ReduceFrameCookies reduce_;
  Context context_;
};

TEST_P(ReduceFrameCookiesTest, RejectBeforeStart) {
  auto packet = CreateStartPacket(GetParam(), kTimestampA, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_FALSE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, AcceptAtStart) {
  auto packet = CreateStartPacket(GetParam(), kTimestampB, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_TRUE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, AcceptBetweenStartAndEnd) {
  auto packet = CreateStartPacket(GetParam(), kTimestampC, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_TRUE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, AcceptAtEnd) {
  auto packet = CreateStartPacket(GetParam(), kTimestampD, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_TRUE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, RejectAfterEnd) {
  auto packet = CreateStartPacket(GetParam(), kTimestampE, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();
  CollectCookies(bytes, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_FALSE(context_.package_frame_cookies.count(kCookieA));
}

INSTANTIATE_TEST_SUITE_P(Default,
                         ReduceFrameCookiesTest,
                         testing::Values(FrameCookieType::ExpectedSurface,
                                         FrameCookieType::ExpectedDisplay,
                                         FrameCookieType::ActualSurface,
                                         FrameCookieType::ActualDisplay));

class TransformStartCookiesTest
    : public testing::Test,
      public testing::WithParamInterface<FrameCookieType> {
 protected:
  void SetUp() { context_.package_frame_cookies.insert(kCookieA); }

  FilterFrameEvents filter_;
  Context context_;
};

TEST_P(TransformStartCookiesTest, RetainStartEvent) {
  auto packet = CreateStartPacket(GetParam(), kTimestampE, kPidA, kCookieA);
  auto bytes = packet.SerializeAsString();

  ASSERT_OK(filter_.Transform(context_, &bytes));

  protos::gen::TracePacket redacted;
  ASSERT_TRUE(redacted.ParseFromString(bytes));

  ASSERT_TRUE(redacted.has_frame_timeline_event());
  const auto& timeline = redacted.frame_timeline_event();

  int64_t cookie;

  // Find the cookie from the packet.
  switch (GetParam()) {
    case FrameCookieType::ExpectedSurface: {
      ASSERT_TRUE(timeline.has_expected_surface_frame_start());
      const auto& start = timeline.expected_surface_frame_start();
      ASSERT_TRUE(start.has_cookie());
      cookie = start.cookie();

      break;
    }

    case FrameCookieType::ExpectedDisplay: {
      ASSERT_TRUE(timeline.has_expected_display_frame_start());
      const auto& start = timeline.expected_display_frame_start();
      ASSERT_TRUE(start.has_cookie());
      cookie = start.cookie();

      break;
    }

    case FrameCookieType::ActualSurface: {
      ASSERT_TRUE(timeline.has_actual_surface_frame_start());
      const auto& start = timeline.actual_surface_frame_start();
      ASSERT_TRUE(start.has_cookie());
      cookie = start.cookie();

      break;
    }
    case FrameCookieType::ActualDisplay: {
      ASSERT_TRUE(timeline.has_actual_display_frame_start());
      const auto& start = timeline.actual_display_frame_start();
      ASSERT_TRUE(start.has_cookie());
      cookie = start.cookie();

      break;
    }
  }

  ASSERT_EQ(cookie, kCookieA);
}

TEST_P(TransformStartCookiesTest, DropStartEvent) {
  // Even those this packet is using PidA, because CookieA is not in the package
  // coookie pool, the event should be dropped.
  auto packet = CreateStartPacket(GetParam(), kTimestampE, kPidA, kCookieB);
  auto bytes = packet.SerializeAsString();

  ASSERT_OK(filter_.Transform(context_, &bytes));

  protos::gen::TracePacket redacted;
  ASSERT_TRUE(redacted.ParseFromString(bytes));

  ASSERT_FALSE(redacted.has_frame_timeline_event());
}

INSTANTIATE_TEST_SUITE_P(Default,
                         TransformStartCookiesTest,
                         testing::Values(FrameCookieType::ExpectedSurface,
                                         FrameCookieType::ExpectedDisplay,
                                         FrameCookieType::ActualSurface,
                                         FrameCookieType::ActualDisplay));

class TransformEndFrameCookiesTest : public testing::Test {
 protected:
  void SetUp() { context_.package_frame_cookies.insert(kCookieA); }

  FilterFrameEvents filter_;
  Context context_;
};

// An end event has no pid. The cookie connects it to a pid. If the start event
// cookie moved from the global pool into the package pool, then end the end
// event should be retained.
TEST_F(TransformStartCookiesTest, Retain) {
  auto packet = CreateFrameEnd(kTimestampA, kCookieA);
  auto bytes = packet.SerializeAsString();

  ASSERT_OK(filter_.Transform(context_, &bytes));

  protos::gen::TracePacket redacted;
  ASSERT_TRUE(redacted.ParseFromString(bytes));

  ASSERT_TRUE(redacted.has_frame_timeline_event());
  const auto& timeline = redacted.frame_timeline_event();

  ASSERT_TRUE(timeline.has_frame_end());
  const auto& end = timeline.frame_end();

  ASSERT_EQ(end.cookie(), kCookieA);
}

TEST_F(TransformStartCookiesTest, Drop) {
  auto packet = CreateFrameEnd(kTimestampA, kCookieB);
  auto bytes = packet.SerializeAsString();

  ASSERT_OK(filter_.Transform(context_, &bytes));

  protos::gen::TracePacket redacted;
  ASSERT_TRUE(redacted.ParseFromString(bytes));

  ASSERT_FALSE(redacted.has_frame_timeline_event());
}

}  // namespace
}  // namespace perfetto::trace_redaction
