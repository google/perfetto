
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
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/frame_timeline_event.gen.h"
#include "protos/perfetto/trace/android/frame_timeline_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

constexpr uint64_t kTimestampA = 0;
constexpr uint64_t kTimestampB = 1000;
constexpr uint64_t kTimestampC = 2000;
constexpr uint64_t kTimestampD = 3000;
constexpr uint64_t kTimestampE = 3000;

constexpr int64_t kCookieA = 1234;

// Start at 1, amd not zero, because zero hnas special meaning (system uid).
constexpr uint64_t kUidA = 1;

constexpr int32_t kPidNone = 10;
constexpr int32_t kPidA = 11;

}  // namespace

class FrameCookieFixture {
 protected:
  std::string CreateStartEvent(int32_t field_id,
                               uint64_t ts,
                               int32_t pid,
                               int64_t cookie) const {
    protos::gen::TracePacket packet;
    packet.set_timestamp(ts);

    switch (field_id) {
      case protos::pbzero::FrameTimelineEvent::
          kExpectedSurfaceFrameStartFieldNumber:
        CreateExpectedSurfaceFrameStart(pid, cookie,
                                        packet.mutable_frame_timeline_event());
        break;

      case protos::pbzero::FrameTimelineEvent::
          kActualSurfaceFrameStartFieldNumber:
        CreateActualSurfaceFrameStart(pid, cookie,
                                      packet.mutable_frame_timeline_event());
        break;

      case protos::pbzero::FrameTimelineEvent::
          kExpectedDisplayFrameStartFieldNumber:
        CreateExpectedDisplayFrameStart(pid, cookie,
                                        packet.mutable_frame_timeline_event());
        break;

      case protos::pbzero::FrameTimelineEvent::
          kActualDisplayFrameStartFieldNumber:
        CreateActualDisplayFrameStart(pid, cookie,
                                      packet.mutable_frame_timeline_event());
        break;

      default:
        PERFETTO_FATAL("Invalid field id");
        break;
    }

    return packet.SerializeAsString();
  }

  std::string CreateFrameEnd(uint64_t ts, int64_t cookie) const {
    protos::gen::TracePacket packet;
    packet.set_timestamp(ts);

    auto* start = packet.mutable_frame_timeline_event()->mutable_frame_end();
    start->set_cookie(cookie);

    return packet.SerializeAsString();
  }

  void CollectEvents(std::initializer_list<ProcessThreadTimeline::Event> events,
                     Context* context) const {
    CollectTimelineEvents collect;
    ASSERT_OK(collect.Begin(context));

    for (const auto& event : events) {
      context->timeline->Append(event);
    }

    ASSERT_OK(collect.End(context));
  }

  void CollectCookies(std::initializer_list<std::string> packets,
                      Context* context) const {
    CollectFrameCookies collect;
    ASSERT_OK(collect.Begin(context));

    for (const auto& packet : packets) {
      protos::pbzero::TracePacket::Decoder decoder(packet);
      ASSERT_OK(collect.Collect(decoder, context));
    }

    ASSERT_OK(collect.End(context));
  }

 private:
  void CreateExpectedSurfaceFrameStart(
      int32_t pid,
      int64_t cookie,
      protos::gen::FrameTimelineEvent* event) const {
    auto* start = event->mutable_expected_surface_frame_start();
    start->set_cookie(cookie);
    start->set_pid(pid);
  }

  void CreateActualSurfaceFrameStart(
      int32_t pid,
      int64_t cookie,
      protos::gen::FrameTimelineEvent* event) const {
    auto* start = event->mutable_actual_surface_frame_start();
    start->set_cookie(cookie);
    start->set_pid(pid);
  }

  void CreateExpectedDisplayFrameStart(
      int32_t pid,
      int64_t cookie,
      protos::gen::FrameTimelineEvent* event) const {
    auto* start = event->mutable_expected_display_frame_start();
    start->set_cookie(cookie);
    start->set_pid(pid);
  }

  void CreateActualDisplayFrameStart(
      int32_t pid,
      int64_t cookie,
      protos::gen::FrameTimelineEvent* event) const {
    auto* start = event->mutable_actual_display_frame_start();
    start->set_cookie(cookie);
    start->set_pid(pid);
  }
};

class CollectFrameCookiesTest : public testing::Test,
                                protected FrameCookieFixture,
                                public testing::WithParamInterface<int32_t> {
 protected:
  Context context_;
};

TEST_P(CollectFrameCookiesTest, ExtractsExpectedSurfaceFrameStart) {
  auto field_id = GetParam();

  auto packet = CreateStartEvent(field_id, kTimestampA, kPidA, kCookieA);

  CollectCookies({packet}, &context_);

  ASSERT_EQ(context_.global_frame_cookies.size(), 1u);

  auto& cookie = context_.global_frame_cookies.back();
  ASSERT_EQ(cookie.cookie, kCookieA);
  ASSERT_EQ(cookie.pid, kPidA);
  ASSERT_EQ(cookie.ts, kTimestampA);
}

INSTANTIATE_TEST_SUITE_P(
    EveryStartEventType,
    CollectFrameCookiesTest,
    testing::Values(
        protos::pbzero::FrameTimelineEvent::
            kExpectedSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::kActualSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kExpectedDisplayFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kActualDisplayFrameStartFieldNumber));

// End events have no influence during the collect phase because they don't have
// a direct connection to a process. They're indirectly connected to a pid via a
// start event (via a common cookie value).
TEST_F(CollectFrameCookiesTest, IgnoresFrameEnd) {
  CollectCookies({CreateFrameEnd(kTimestampA, kPidA)}, &context_);

  ASSERT_TRUE(context_.global_frame_cookies.empty());
}

class ReduceFrameCookiesTest : public testing::Test,
                               protected FrameCookieFixture,
                               public testing::WithParamInterface<int32_t> {
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
    CollectEvents(
        {
            ProcessThreadTimeline::Event::Open(kTimestampB, kPidA, kPidNone,
                                               kUidA),
            ProcessThreadTimeline::Event::Close(kTimestampD, kPidA),
        },
        &context_);
  }

  ReduceFrameCookies reduce_;
  Context context_;
};

TEST_P(ReduceFrameCookiesTest, RejectBeforeActive) {
  auto field_id = GetParam();

  // kTimestampA is before pid starts.
  auto packet = CreateStartEvent(field_id, kTimestampA, kPidA, kCookieA);

  CollectCookies({packet}, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_FALSE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, AcceptDuringActive) {
  auto field_id = GetParam();

  // kTimestampC is between pid starts and ends.
  auto packet = CreateStartEvent(field_id, kTimestampC, kPidA, kCookieA);

  CollectCookies({packet}, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_TRUE(context_.package_frame_cookies.count(kCookieA));
}

TEST_P(ReduceFrameCookiesTest, RejectAfterActive) {
  auto field_id = GetParam();

  // kTimestampE is after pid ends.
  auto packet = CreateStartEvent(field_id, kTimestampE, kPidA, kCookieA);

  CollectCookies({packet}, &context_);

  ASSERT_OK(reduce_.Build(&context_));
  ASSERT_FALSE(context_.package_frame_cookies.count(kCookieA));
}

INSTANTIATE_TEST_SUITE_P(
    EveryStartEventType,
    ReduceFrameCookiesTest,
    testing::Values(
        protos::pbzero::FrameTimelineEvent::
            kExpectedSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::kActualSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kExpectedDisplayFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kActualDisplayFrameStartFieldNumber));

class FilterCookiesFieldsTest : public testing::Test,
                                protected FrameCookieFixture,
                                public testing::WithParamInterface<int32_t> {
 protected:
  protozero::Field ExtractTimelineEvent(const std::string& packet) const {
    protozero::ProtoDecoder packet_decoder(packet);

    // There must be one in order for the test to work, so we assume it's there.
    return packet_decoder.FindField(
        protos::pbzero::TracePacket::kFrameTimelineEventFieldNumber);
  }

  FilterFrameEvents filter_;
  Context context_;
};

// If the event was within a valid pid's lifespan and was connected to the
// package, it should be kept.
TEST_P(FilterCookiesFieldsTest, IncludeIncludedStartCookies) {
  context_.package_frame_cookies.insert(kCookieA);

  auto field_id = GetParam();
  auto packet = CreateStartEvent(field_id, kTimestampA, kPidA, kCookieA);
  auto timeline_field = ExtractTimelineEvent(packet);

  ASSERT_TRUE(filter_.KeepField(context_, timeline_field));
}

// If the event wasn't within a valid pid's lifespans and/or was connected to a
// package, it should be removed.
TEST_P(FilterCookiesFieldsTest, ExcludeMissingStartCookies) {
  auto field_id = GetParam();
  auto packet = CreateStartEvent(field_id, kTimestampA, kPidA, kCookieA);
  auto timeline_field = ExtractTimelineEvent(packet);

  ASSERT_FALSE(filter_.KeepField(context_, timeline_field));
}

INSTANTIATE_TEST_SUITE_P(
    EveryStartEventType,
    FilterCookiesFieldsTest,
    testing::Values(
        protos::pbzero::FrameTimelineEvent::
            kExpectedSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::kActualSurfaceFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kExpectedDisplayFrameStartFieldNumber,
        protos::pbzero::FrameTimelineEvent::
            kActualDisplayFrameStartFieldNumber));

TEST_F(FilterCookiesFieldsTest, IncludeIncludedEndCookies) {
  context_.package_frame_cookies.insert(kCookieA);

  auto packet = CreateFrameEnd(kTimestampA, kCookieA);
  auto timeline_field = ExtractTimelineEvent(packet);

  ASSERT_TRUE(filter_.KeepField(context_, timeline_field));
}

TEST_F(FilterCookiesFieldsTest, ExcludeMissingEndCookies) {
  auto packet = CreateFrameEnd(kTimestampA, kCookieA);
  auto timeline_field = ExtractTimelineEvent(packet);

  ASSERT_FALSE(filter_.KeepField(context_, timeline_field));
}

}  // namespace perfetto::trace_redaction
