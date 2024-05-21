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

#include "src/trace_redaction/redact_sched_switch.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {

constexpr uint64_t kUidA = 1;
constexpr uint64_t kUidB = 2;
constexpr uint64_t kUidC = 3;

constexpr int32_t kNoParent = 10;
constexpr int32_t kPidA = 11;
constexpr int32_t kPidB = 12;

constexpr int32_t kCpuA = 0;

constexpr uint64_t kFullStep = 1000;
constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = kFullStep;

constexpr auto kCommA = "comm-a";
constexpr auto kCommB = "comm-b";
constexpr auto kCommNone = "";

}  // namespace

class RedactSchedSwitchTest : public testing::Test {
 protected:
  void SetUp() override {
    // Create a packet where two pids are swapping back-and-forth.
    auto* bundle = packet_.mutable_ftrace_events();
    bundle->set_cpu(kCpuA);

    {
      auto* event = bundle->add_event();

      event->set_timestamp(kTimeA);
      event->set_pid(kPidA);

      auto* sched_switch = event->mutable_sched_switch();
      sched_switch->set_prev_comm(kCommA);
      sched_switch->set_prev_pid(kPidA);
      sched_switch->set_prev_prio(0);
      sched_switch->set_prev_state(0);
      sched_switch->set_next_comm(kCommB);
      sched_switch->set_next_pid(kPidB);
      sched_switch->set_next_prio(0);
    }

    {
      auto* event = bundle->add_event();

      event->set_timestamp(kTimeB);
      event->set_pid(kPidB);

      auto* sched_switch = event->mutable_sched_switch();
      sched_switch->set_prev_comm(kCommB);
      sched_switch->set_prev_pid(kPidB);
      sched_switch->set_prev_prio(0);
      sched_switch->set_prev_state(0);
      sched_switch->set_next_comm(kCommA);
      sched_switch->set_next_pid(kPidA);
      sched_switch->set_next_prio(0);
    }

    // PID A and PID B need to be attached to different packages (UID) so that
    // its possible to include one but not the other.
    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();
  }

  protos::gen::TracePacket packet_;
  Context context_;
};

// In this case, the target uid will be UID A. That means the comm values for
// PID B should be removed, and the comm values for PID A should remain.
TEST_F(RedactSchedSwitchTest, KeepsTargetCommValues) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events[0].sched_switch().prev_pid(), kPidA);
  ASSERT_EQ(events[0].sched_switch().prev_comm(), kCommA);

  ASSERT_EQ(events[0].sched_switch().next_pid(), kPidB);
  ASSERT_EQ(events[0].sched_switch().next_comm(), kCommNone);

  ASSERT_EQ(events[1].sched_switch().prev_pid(), kPidB);
  ASSERT_EQ(events[1].sched_switch().prev_comm(), kCommNone);

  ASSERT_EQ(events[1].sched_switch().next_pid(), kPidA);
  ASSERT_EQ(events[1].sched_switch().next_comm(), kCommA);
}

// This case is very similar to the "some are connected", expect that it
// verifies all comm values will be removed when testing against an unused
// uid.
TEST_F(RedactSchedSwitchTest, RemovesAllCommsIfPackageDoesntExist) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  context_.package_uid = kUidC;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events[0].sched_switch().prev_comm(), kCommNone);
  ASSERT_EQ(events[0].sched_switch().next_comm(), kCommNone);

  ASSERT_EQ(events[1].sched_switch().prev_comm(), kCommNone);
  ASSERT_EQ(events[1].sched_switch().next_comm(), kCommNone);
}

}  // namespace perfetto::trace_redaction
