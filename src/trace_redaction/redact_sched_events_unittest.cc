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

#include "src/trace_redaction/redact_sched_events.h"
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
constexpr int32_t kPidD = 14;

constexpr int32_t kCpuA = 0;
constexpr int32_t kCpuB = 1;
constexpr int32_t kCpuC = 2;

constexpr uint64_t kHalfStep = 500;
constexpr uint64_t kFullStep = kHalfStep * 2;

constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = kFullStep;
constexpr uint64_t kTimeC = kFullStep * 2;

constexpr auto kCommA = "comm-a";
constexpr auto kCommB = "comm-b";
constexpr auto kCommC = "comm-c";
constexpr auto kCommNone = "";

template <int32_t new_pid>
class ChangePidTo : public PidCommModifier {
 public:
  void Modify(const Context& context,
              uint64_t ts,
              int32_t,
              int32_t* pid,
              std::string*) const override {
    PERFETTO_DCHECK(context.timeline);
    PERFETTO_DCHECK(context.package_uid.has_value());
    PERFETTO_DCHECK(pid);
    if (!context.timeline->PidConnectsToUid(ts, *pid, *context.package_uid)) {
      *pid = new_pid;
    }
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

    redact_.emplace_modifier<ClearComms>();
    redact_.emplace_waking_filter<AllowAll>();
  }

  protos::gen::TracePacket packet_;
  Context context_;
  RedactSchedEvents redact_;
};

// In this case, the target uid will be UID A. That means the comm values for
// PID B should be removed, and the comm values for PID A should remain.
TEST_F(RedactSchedSwitchFtraceEventTest, KeepsTargetCommValues) {
  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

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
  context_.package_uid = kUidC;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

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

    redact_.emplace_modifier<ClearComms>();
    redact_.emplace_waking_filter<AllowAll>();
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
  RedactSchedEvents redact_;
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

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

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

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

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

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

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

  redact_.emplace_modifier<ChangePidTo<kPidC>>();

  ASSERT_OK(redact_.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  ASSERT_TRUE(bundle.has_compact_sched());

  const auto& compact_sched = bundle.compact_sched();

  // The intern table should not change.
  ASSERT_EQ(compact_sched.intern_table_size(), 2);

  ASSERT_EQ(compact_sched.switch_next_pid_size(), 2);
  ASSERT_EQ(compact_sched.switch_next_pid().at(0), kPidA);

  // Because Pid B was not connected to Uid A, it should have its pid changed.
  ASSERT_EQ(compact_sched.switch_next_pid().at(1), kPidC);
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

    redact.emplace_modifier<ClearComms>();
    redact.emplace_waking_filter<AllowAll>();
  }

  protos::gen::TracePacket packet_;
  Context context_;

  RedactSchedEvents redact;
};

TEST_F(RedactSchedWakingFtraceEventTest, WakeeKeepsCommWhenConnectedToPackage) {
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
  redact.emplace_modifier<ChangePidTo<kPidD>>();

  context_.package_uid = kUidB;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  ASSERT_EQ(events.front().sched_waking().pid(), kPidB);

  // Because Pid C was not connected to Uid B, it should have its pid changed.
  ASSERT_EQ(events.back().sched_waking().pid(), kPidD);
}

TEST_F(RedactSchedWakingFtraceEventTest,
       WakeeLosesPidWhenNotConnectedToPackage) {
  redact.emplace_modifier<ChangePidTo<kPidD>>();

  context_.package_uid = kUidA;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  // Both pids should have changed.
  ASSERT_EQ(events.at(0).sched_waking().pid(), kPidD);
  ASSERT_EQ(events.at(1).sched_waking().pid(), kPidD);
}

TEST_F(RedactSchedWakingFtraceEventTest, WakerPidIsLeftUnaffected) {
  redact.emplace_modifier<ChangePidTo<kPidD>>();

  context_.package_uid = kUidB;

  auto packet_buffer = packet_.SerializeAsString();

  ASSERT_OK(redact.Transform(context_, &packet_buffer));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_buffer));

  const auto& bundle = packet.ftrace_events();
  const auto& events = bundle.event();

  ASSERT_EQ(events.size(), 2u);

  // The waker in the ftrace event waking event should change, but by another
  // primitive. This case only appears in the ftrace events because the waker is
  // inferred in the comp sched case.
  ASSERT_EQ(events.at(0).pid(), static_cast<uint32_t>(kPidA));
  ASSERT_EQ(events.at(1).pid(), static_cast<uint32_t>(kPidA));
}

class FilterCompactSchedWakingEventsTest : public testing::Test {
 protected:
  void SetUp() {
    // Uid B is used instead of Uid A because Pid A, belonging to Uid A, is the
    // waker. Pid B and Pid C are the wakees.
    context_.package_uid = kUidB;

    // FilterSchedWakingEvents expects a timeline because most
    // FilterSchedWakingEvents::Filter filters will need one. However, the
    // filter used in this test doesn't require one.
    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidC, kNoParent, kUidC));
    context_.timeline->Sort();

    // Default to "allow all" and "change nothing" so a test only needs to
    // override what they need.
    redact_.emplace_waking_filter<AllowAll>();
    redact_.emplace_modifier<DoNothing>();
  }

  Context context_;
  RedactSchedEvents redact_;
};

// Builds a simple ftrace bundle that contains two ftrace events:
//
//  - Pid A wakes up pid B
//  - Pid A wakes up pid C
//
// Because compact sched uses associative arrays, the data will look like:
//
//  - Time | PID   | CPU   | *
//    -----+-------+-------+---
//    0.5  | kPidB | kCpuB |
//    1.5  | kPidC | kCpuB |
//
// Because the filter will only keep events where pid is being waked, only the
// first of the two events should remain.
TEST_F(FilterCompactSchedWakingEventsTest, FilterCompactSched) {
  redact_.emplace_waking_filter<ConnectedToPackage>();

  protos::gen::TracePacket packet_builder;
  packet_builder.mutable_ftrace_events()->set_cpu(kCpuA);

  auto* compact_sched =
      packet_builder.mutable_ftrace_events()->mutable_compact_sched();

  compact_sched->add_intern_table(kCommA);

  // Implementation detail: The timestamp, target cpu, and pid matter. The other
  // values are copied to the output, but have no influence over the internal
  // logic.
  compact_sched->add_waking_comm_index(0);
  compact_sched->add_waking_common_flags(0);
  compact_sched->add_waking_prio(0);
  compact_sched->add_waking_timestamp(kHalfStep);
  compact_sched->add_waking_target_cpu(kCpuB);
  compact_sched->add_waking_pid(kPidB);

  compact_sched->add_waking_comm_index(0);
  compact_sched->add_waking_common_flags(0);
  compact_sched->add_waking_prio(0);
  compact_sched->add_waking_timestamp(kFullStep + kHalfStep);
  compact_sched->add_waking_target_cpu(kCpuB);
  compact_sched->add_waking_pid(kPidC);

  auto bytes = packet_builder.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &bytes));

  protos::gen::TracePacket packet;
  packet.ParseFromString(bytes);

  ASSERT_TRUE(packet.has_ftrace_events());

  const auto& events = packet.ftrace_events();
  ASSERT_TRUE(events.has_compact_sched());

  // All events not from Pid B should be removed. In this case, that means the
  // event from Pid C should be dropped.
  ASSERT_EQ(events.compact_sched().waking_pid_size(), 1);
  ASSERT_EQ(events.compact_sched().waking_pid().at(0), kPidB);
}

// Timing information is based off delta-time values. When a row is removed
// from the compact sched arrays, downstream timing data is corrupted. The
// delta value of removed rows should be rolled into the next row.
TEST_F(FilterCompactSchedWakingEventsTest,
       CorrectsTimeWhenRemovingWakingEvents) {
  // All the times are delta times. The commented times are the absolute times.
  std::array<uint64_t, 7> before = {
      0,
      kFullStep,  // 1
      kFullStep,  // 2
      kHalfStep,  // 2.5
      kHalfStep,  // 3
      kFullStep,  // 4
      kFullStep,  // 5
  };

  // These are the times that should be drop
  std::array<uint64_t, 3> drop_times = {
      kFullStep,  // 6
      kFullStep,  // 7
      kHalfStep,  // 7.5
  };

  // When the times are dropped, the times removed from drop_times should be
  // rolling into the first time. So it should got from 1 unit to 3.5 units.
  std::array<uint64_t, 2> after = {
      kFullStep,  // 8
      kFullStep,  // 9
  };

  protos::gen::TracePacket packet_builder;
  packet_builder.mutable_ftrace_events()->set_cpu(kCpuA);

  auto* compact_sched =
      packet_builder.mutable_ftrace_events()->mutable_compact_sched();

  compact_sched->add_intern_table(kCommA);

  // Before and after, this events should not be affected.
  for (auto time : before) {
    compact_sched->add_waking_comm_index(0);
    compact_sched->add_waking_common_flags(0);
    compact_sched->add_waking_prio(0);
    compact_sched->add_waking_timestamp(time);
    compact_sched->add_waking_target_cpu(kCpuB);
    compact_sched->add_waking_pid(kPidB);
  }

  // Use pid B so that these times will be dropped.
  for (auto time : drop_times) {
    compact_sched->add_waking_comm_index(0);
    compact_sched->add_waking_common_flags(0);
    compact_sched->add_waking_prio(0);
    compact_sched->add_waking_timestamp(time);
    compact_sched->add_waking_target_cpu(kCpuB);
    compact_sched->add_waking_pid(kPidC);
  }

  // After redaction, these events should still exist, but the first event in
  // this series, the timestamp should be larger (before of the dropped events).
  for (auto time : after) {
    compact_sched->add_waking_comm_index(0);
    compact_sched->add_waking_common_flags(0);
    compact_sched->add_waking_prio(0);
    compact_sched->add_waking_timestamp(time);
    compact_sched->add_waking_target_cpu(kCpuB);
    compact_sched->add_waking_pid(kPidB);
  }

  auto bytes = packet_builder.SerializeAsString();

  redact_.emplace_waking_filter<ConnectedToPackage>();
  ASSERT_OK(redact_.Transform(context_, &bytes));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(bytes));

  ASSERT_TRUE(packet.has_ftrace_events());
  const auto& events = packet.ftrace_events();

  ASSERT_TRUE(events.has_compact_sched());
  const auto& times = packet.ftrace_events().compact_sched().waking_timestamp();

  ASSERT_EQ(times.size(), 9u);  // i.e. before + after

  // Nothing in the before should have changed.
  for (size_t i = 0; i < before.size(); ++i) {
    ASSERT_EQ(times[i], before[i]);
  }

  // Sum of all dropped event time.
  ASSERT_EQ(drop_times.size(), 3u);
  auto lost_time = drop_times[0] + drop_times[1] + drop_times[2];

  // Only the first of the two "after" events should have changed.
  ASSERT_EQ(times[before.size()], after[0] + lost_time);
  ASSERT_EQ(times[before.size() + 1], after[1]);
}

// This is an implementation detail. When an event is removed, the gap is
// collapsed into the next event by tracking the error created by removing the
// event. If implemented incorrectly, flipping between keep and remove will
// break as the error will not be reset correctly.
TEST_F(FilterCompactSchedWakingEventsTest, RemovingWakingEventsThrashing) {
  //   X  : Drop this event
  //  [ ] : This is an event
  //   =  : Number of time units
  //
  //           X          X          X
  //  [==][==][=][==][==][=][==][==][=]
  //
  // Events are going to follow a "keep, keep, drop" pattern. All keep events
  // will be full time units. All drop events will be half time units.
  //
  // It is key to notice that the series ends on a removed event. This creates a
  // special: remove an event without an event to accept the error.
  std::array<uint64_t, 9> before = {
      0,          // abs time 0
      kFullStep,  // abs time 1
      kHalfStep,  // abs time 1.5

      kFullStep,  // abs time 2.5
      kFullStep,  // abs time 3.5
      kHalfStep,  // abs time 4

      kFullStep,  // abs time 5
      kFullStep,  // abs time 6
      kHalfStep,  // abs time 6.5
  };

  std::array<uint64_t, 6> after = {
      0,                      // abs time 0
      kFullStep,              // abs time 1
      kFullStep + kHalfStep,  // abs time 2.5
      kFullStep,              // abs time 3.5
      kFullStep + kHalfStep,  // abs time 5
      kFullStep,              // abs time 6
  };

  protos::gen::TracePacket packet_builder;
  packet_builder.mutable_ftrace_events()->set_cpu(kCpuA);

  auto* compact_sched =
      packet_builder.mutable_ftrace_events()->mutable_compact_sched();

  compact_sched->add_intern_table(kCommA);

  for (size_t i = 0; i < before.size(); ++i) {
    auto time = before[i];

    compact_sched->add_waking_comm_index(0);
    compact_sched->add_waking_common_flags(0);
    compact_sched->add_waking_prio(0);
    compact_sched->add_waking_timestamp(time);
    compact_sched->add_waking_target_cpu(kCpuB);

    // The pattern is "keep, keep, drop", therefore, PID B > B > C ...
    if (i % 3 == 2) {
      compact_sched->add_waking_pid(kPidC);
    } else {
      compact_sched->add_waking_pid(kPidB);
    }
  }

  auto bytes = packet_builder.SerializeAsString();

  redact_.emplace_waking_filter<ConnectedToPackage>();
  ASSERT_OK(redact_.Transform(context_, &bytes));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(bytes));

  ASSERT_TRUE(packet.has_ftrace_events());
  const auto& events = packet.ftrace_events();

  ASSERT_TRUE(events.has_compact_sched());
  const auto& times = packet.ftrace_events().compact_sched().waking_timestamp();

  ASSERT_EQ(times.size(), after.size());

  for (size_t i = 0; i < after.size(); ++i) {
    ASSERT_EQ(times[i], after[i]);
  }
}

}  // namespace perfetto::trace_redaction
