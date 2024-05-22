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
constexpr int32_t kPidC = 13;

constexpr int32_t kCpuA = 0;
constexpr int32_t kCpuB = 1;
constexpr int32_t kCpuC = 2;

constexpr uint64_t kFullStep = 1000;
constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = kFullStep;
constexpr uint64_t kTimeC = kFullStep * 2;

constexpr auto kCommA = "comm-a";
constexpr auto kCommB = "comm-b";
constexpr auto kCommC = "comm-c";
constexpr auto kCommNone = "";

class ChangePidToMax : public RedactSchedSwitchHarness::Modifier {
 public:
  base::Status Modify(const Context& context,
                      uint64_t ts,
                      int32_t,
                      int32_t* pid,
                      std::string*) const override {
    if (!context.timeline->PidConnectsToUid(ts, *pid, *context.package_uid)) {
      *pid = std::numeric_limits<int32_t>::max();
    }

    return base::OkStatus();
  }
};

}  // namespace

class RedactSchedSwitchFtraceEventTest : public testing::Test {
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
TEST_F(RedactSchedSwitchFtraceEventTest, KeepsTargetCommValues) {
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
TEST_F(RedactSchedSwitchFtraceEventTest, RemovesAllCommsIfPackageDoesntExist) {
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

class RedactCompactSchedSwitchTest : public testing::Test {
 protected:
  void SetUp() override {
    // PID A and PID B need to be attached to different packages (UID) so that
    // its possible to include one but not the other.
    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    auto* bundle = packet_.mutable_ftrace_events();
    bundle->set_cpu(kCpuA);  // All switch events occur on this CPU

    compact_sched = bundle->mutable_compact_sched();

    compact_sched->add_intern_table(kCommA);
    compact_sched->add_intern_table(kCommB);
  }

  void AddSwitchEvent(uint64_t ts,
                      int32_t next_pid,
                      int32_t prev_state,
                      int32_t prio,
                      uint32_t comm) {
    compact_sched->add_switch_timestamp(ts);
    compact_sched->add_switch_next_pid(next_pid);
    compact_sched->add_switch_prev_state(prev_state);
    compact_sched->add_switch_next_prio(prio);
    compact_sched->add_switch_next_comm_index(comm);
  }

  protos::gen::TracePacket packet_;
  protos::gen::FtraceEventBundle::CompactSched* compact_sched;

  Context context_;
};

TEST_F(RedactCompactSchedSwitchTest, KeepsTargetCommValues) {
  uint32_t kCommIndexA = 0;
  uint32_t kCommIndexB = 1;

  // The new entry will be appended to the table. Another primitive can be used
  // to reduce the intern string table.
  uint32_t kCommIndexNone = 2;

  AddSwitchEvent(kTimeA, kPidA, 0, 0, kCommIndexA);
  AddSwitchEvent(kTimeB, kPidB, 0, 0, kCommIndexB);

  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  ASSERT_TRUE(bundle.has_compact_sched());

  const auto& compact_sched = bundle.compact_sched();

  // A new entry (empty string) should have been added to the table.
  ASSERT_EQ(compact_sched.intern_table_size(), 3);
  ASSERT_EQ(compact_sched.intern_table().back(), kCommNone);

  ASSERT_EQ(compact_sched.switch_next_comm_index_size(), 2);
  ASSERT_EQ(compact_sched.switch_next_comm_index().at(0), kCommIndexA);
  ASSERT_EQ(compact_sched.switch_next_comm_index().at(1), kCommIndexNone);
}

// If two pids use the same comm, but one pid changes, the shared comm should
// still be available.
TEST_F(RedactCompactSchedSwitchTest, ChangingSharedCommonRetainsComm) {
  uint32_t kCommIndexA = 0;

  AddSwitchEvent(kTimeA, kPidA, 0, 0, kCommIndexA);
  AddSwitchEvent(kTimeB, kPidB, 0, 0, kCommIndexA);

  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  ASSERT_TRUE(bundle.has_compact_sched());

  const auto& compact_sched = bundle.compact_sched();

  // A new entry should have been appended, but comm A (previously shared)
  // should still exist in the table.
  ASSERT_EQ(compact_sched.intern_table_size(), 3);
  ASSERT_EQ(compact_sched.intern_table().front(), kCommA);
  ASSERT_EQ(compact_sched.intern_table().back(), kCommNone);
}

TEST_F(RedactCompactSchedSwitchTest, RemovesAllCommsIfPackageDoesntExist) {
  uint32_t kCommIndexA = 0;
  uint32_t kCommIndexB = 1;

  // The new entry will be appended to the table. Another primitive can be used
  // to reduce the intern string table.
  uint32_t kCommIndexNone = 2;

  AddSwitchEvent(kTimeA, kPidA, 0, 0, kCommIndexA);
  AddSwitchEvent(kTimeB, kPidB, 0, 0, kCommIndexB);

  context_.package_uid = kUidC;

  auto packet_buffer = packet_.SerializeAsString();

  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  ASSERT_TRUE(bundle.has_compact_sched());

  const auto& compact_sched = bundle.compact_sched();

  // A new entry (empty string) should have been added to the table.
  ASSERT_EQ(compact_sched.intern_table_size(), 3);
  ASSERT_EQ(compact_sched.intern_table().back(), kCommNone);

  ASSERT_EQ(compact_sched.switch_next_comm_index_size(), 2);
  ASSERT_EQ(compact_sched.switch_next_comm_index().at(0), kCommIndexNone);
  ASSERT_EQ(compact_sched.switch_next_comm_index().at(1), kCommIndexNone);
}

TEST_F(RedactCompactSchedSwitchTest, CanChangePid) {
  uint32_t kCommIndexA = 0;
  uint32_t kCommIndexB = 1;

  AddSwitchEvent(kTimeA, kPidA, 0, 0, kCommIndexA);
  AddSwitchEvent(kTimeB, kPidB, 0, 0, kCommIndexB);

  // Because the target is package A, PidA should be remain. PidB should change.
  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ChangePidToMax>();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  ASSERT_TRUE(bundle.has_compact_sched());

  const auto& compact_sched = bundle.compact_sched();

  // The intern table should not change.
  ASSERT_EQ(compact_sched.intern_table_size(), 2);

  ASSERT_EQ(compact_sched.switch_next_pid_size(), 2);
  ASSERT_EQ(compact_sched.switch_next_pid().at(0), kPidA);
  ASSERT_NE(compact_sched.switch_next_pid().at(1), kPidB);
}

class RedactSchedWakingFtraceEventTest : public testing::Test {
 protected:
  void SetUp() override {
    // Create a packet where two pids are swapping back-and-forth.
    auto* bundle = packet_.mutable_ftrace_events();
    bundle->set_cpu(kCpuA);

    // Pid A wakes up Pid B at time Time B
    {
      auto* event = bundle->add_event();

      event->set_timestamp(kTimeB);
      event->set_pid(kPidA);

      auto* sched_waking = event->mutable_sched_waking();
      sched_waking->set_comm(kCommB);
      sched_waking->set_pid(kPidB);
      sched_waking->set_prio(0);
      sched_waking->set_success(true);
      sched_waking->set_target_cpu(kCpuB);
    }

    // Pid A wakes up Pid C at time Time C.
    {
      auto* event = bundle->add_event();

      event->set_timestamp(kTimeC);
      event->set_pid(kPidA);

      auto* sched_waking = event->mutable_sched_waking();
      sched_waking->set_comm(kCommC);
      sched_waking->set_pid(kPidC);
      sched_waking->set_prio(0);
      sched_waking->set_success(true);
      sched_waking->set_target_cpu(kCpuC);
    }

    // PID A and PID B need to be attached to different packages (UID) so that
    // its possible to include one but not the other.
    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidC, kNoParent, kUidC));
    context_.timeline->Sort();
  }

  protos::gen::TracePacket packet_;
  Context context_;
};

TEST_F(RedactSchedWakingFtraceEventTest, WakeeKeepsCommWhenConnectedToPackage) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ClearComms>();

  context_.package_uid = kUidB;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events.front().sched_waking().comm(), kCommB);
  ASSERT_EQ(events.back().sched_waking().comm(), kCommNone);
}

TEST_F(RedactSchedWakingFtraceEventTest,
       WakeeLosesCommWhenNotConnectedToPackage) {
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

  ASSERT_EQ(events.front().sched_waking().comm(), kCommNone);
  ASSERT_EQ(events.back().sched_waking().comm(), kCommNone);
}

TEST_F(RedactSchedWakingFtraceEventTest, WakeeKeepsPidWhenConnectedToPackage) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ChangePidToMax>();

  context_.package_uid = kUidB;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events.front().sched_waking().pid(), kPidB);
  ASSERT_NE(events.back().sched_waking().pid(), kPidC);
}

TEST_F(RedactSchedWakingFtraceEventTest,
       WakeeLosesPidWhenNotConnectedToPackage) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ChangePidToMax>();

  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_NE(events.front().sched_waking().pid(), kPidB);
  ASSERT_NE(events.back().sched_waking().pid(), kPidC);
}

TEST_F(RedactSchedWakingFtraceEventTest, WakerPidIsLeftUnaffected) {
  RedactSchedSwitchHarness redact;
  redact.emplace_transform<ChangePidToMax>();

  context_.package_uid = kUidB;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events.front().pid(), static_cast<uint32_t>(kPidA));
  ASSERT_EQ(events.back().pid(), static_cast<uint32_t>(kPidA));
}

}  // namespace perfetto::trace_redaction
