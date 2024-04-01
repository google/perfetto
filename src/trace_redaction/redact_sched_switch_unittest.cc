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
#include "protos/perfetto/trace/ftrace/power.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
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

constexpr std::string_view kCommA = "comm-a";
constexpr std::string_view kCommB = "comm-b";
constexpr std::string_view kCommC = "comm-c";

constexpr uint64_t kTimeA = 100;
constexpr uint64_t kTimeB = 200;
constexpr uint64_t kTimeC = 300;

}  // namespace

// Tests which nested messages and fields are removed.
class RedactSchedSwitchTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    // Three concurrent processes. No parent. All in different packages.
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(0, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(0, kPidB, kNoParent, kUidB));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(0, kPidC, kNoParent, kUidC));

    context_.timeline->Sort();
  }

  void BeginBundle() { ftrace_bundle_ = trace_packet_.mutable_ftrace_events(); }

  void AddSwitch(uint64_t ts,
                 int32_t prev_pid,
                 std::string_view prev_comm,
                 int32_t next_pid,
                 std::string_view next_comm) {
    ASSERT_NE(ftrace_bundle_, nullptr);

    auto* event = ftrace_bundle_->add_event();
    event->set_timestamp(ts);

    auto* sched_switch = event->mutable_sched_switch();
    sched_switch->set_prev_pid(prev_pid);
    sched_switch->set_prev_comm(std::string(prev_comm));
    sched_switch->set_next_pid(next_pid);
    sched_switch->set_next_comm(std::string(next_comm));
  }

  base::StatusOr<protos::gen::TracePacket> Transform() {
    auto packet = trace_packet_.SerializeAsString();
    auto result = transform_.Transform(context_, &packet);

    if (!result.ok()) {
      return result;
    }

    protos::gen::TracePacket redacted_packet;
    redacted_packet.ParseFromString(packet);

    return redacted_packet;
  }

  Context context_;

  const RedactSchedSwitch& transform() const { return transform_; }

 private:
  protos::gen::TracePacket trace_packet_;
  protos::gen::FtraceEventBundle* ftrace_bundle_;

  RedactSchedSwitch transform_;
};

TEST_F(RedactSchedSwitchTest, ReturnsErrorForNullPacket) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kUidA;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  ASSERT_FALSE(transform().Transform(context, nullptr).ok());
}

TEST_F(RedactSchedSwitchTest, ReturnsErrorForEmptyPacket) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kUidA;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  std::string packet_str = "";

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

TEST_F(RedactSchedSwitchTest, ReturnsErrorForNoTimeline) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kUidA;

  protos::gen::TracePacket packet;
  std::string packet_str = packet.SerializeAsString();

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

TEST_F(RedactSchedSwitchTest, ReturnsErrorForNoPackage) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protos::gen::TracePacket packet;
  std::string packet_str = packet.SerializeAsString();

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

TEST_F(RedactSchedSwitchTest, BundleWithNonEventChild) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.package_uid = 0;

  // packet {
  //   ftrace_events {
  //     cpu: 0
  //     event {
  //       timestamp: 6702093744772646
  //       pid: 0
  //       sched_switch {
  //         prev_comm: "swapper/0"
  //         prev_pid: 0
  //         prev_prio: 120
  //         prev_state: 0
  //         next_comm: "writer"
  //         next_pid: 23020
  //         next_prio: 96
  //       }
  //     }
  //   }
  // }

  protos::gen::TracePacket packet;
  auto* events = packet.mutable_ftrace_events();
  events->set_cpu(0);

  auto* event = events->add_event();
  event->set_timestamp(kPidA);
  event->set_pid(kPidA);

  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_comm("swapper/0");
  sched_switch->set_prev_pid(kPidA);
  sched_switch->set_prev_prio(120);
  sched_switch->set_prev_state(0);

  sched_switch->set_next_comm("writer");
  sched_switch->set_next_pid(kPidB);
  sched_switch->set_next_prio(96);

  std::string packet_str = packet.SerializeAsString();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  protos::gen::TracePacket redacted;
  redacted.ParseFromString(packet_str);

  // Make sure values alongside the "event" value (e.g. "cpu") are retained.
  ASSERT_TRUE(redacted.has_ftrace_events());
  ASSERT_TRUE(redacted.ftrace_events().has_cpu());
}

// There are more than sched_switch events in the ftrace_events message.
// Beyond supporting simple fields along side the event (e.g. cpu), not all
// events will contain sched_switch events. Make sure that all every message is
// retained while redacting the sched_switch.
TEST_F(RedactSchedSwitchTest, KeepsNonSwitchEvents) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = 2;

  // Keep the previous PID and remove the next PID.
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.timeline->Append(ProcessThreadTimeline::Event::Open(0, 0, 1, 2));
  context.timeline->Sort();

  // packet {
  //   ftrace_events {
  //     cpu: 0
  //     event {
  //       timestamp: 6702093744766292
  //       pid: 0
  //       cpu_idle {
  //         state: 4294967295
  //         cpu_id: 0
  //       }
  //     }
  //     event {
  //       timestamp: 6702093744772646
  //       pid: 0
  //       sched_switch {
  //         prev_comm: "swapper/0"
  //         prev_pid: 0
  //         prev_prio: 120
  //         prev_state: 0
  //         next_comm: "writer"
  //         next_pid: 23020
  //         next_prio: 96
  //       }
  //     }
  //     event {
  //       timestamp: 6702093744803376
  //       pid: 23020
  //       sched_waking {
  //         comm: "FastMixer"
  //         pid: 1619
  //         prio: 96
  //         success: 1
  //         target_cpu: 1
  //       }
  //     }
  //   }
  // }

  protos::gen::TracePacket source_packet;
  source_packet.mutable_ftrace_events()->set_cpu(0);

  // cpu_idle
  do {
    auto* event = source_packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(6702093744766292);
    event->set_pid(0);

    auto* cpu_idle = event->mutable_cpu_idle();
    cpu_idle->set_state(4294967295);
    cpu_idle->set_cpu_id(0);
  } while (false);

  // sched_switch
  do {
    auto* event = source_packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(6702093744772646);
    event->set_pid(0);

    auto* sched_switch = event->mutable_sched_switch();
    sched_switch->set_prev_comm("swapper/0");
    sched_switch->set_prev_pid(0);
    sched_switch->set_prev_prio(120);
    sched_switch->set_prev_state(0);
    sched_switch->set_next_comm("writer");
    sched_switch->set_next_pid(23020);
    sched_switch->set_next_prio(96);
  } while (false);

  // sched_waking
  do {
    auto* event = source_packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(6702093744803376);
    event->set_pid(23020);

    auto* sched_waking = event->mutable_sched_waking();
    sched_waking->set_comm("FastMixer");
    sched_waking->set_pid(1619);
    sched_waking->set_prio(96);
    sched_waking->set_success(1);
    sched_waking->set_target_cpu(1);
  } while (false);

  auto packet_str = source_packet.SerializeAsString();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  protos::gen::TracePacket packet;
  source_packet.ParseFromString(packet_str);

  // Make sure values alongside the "event" value (e.g. "cpu") are retained.
  ASSERT_TRUE(source_packet.has_ftrace_events());

  auto& ftrace_packets = source_packet.ftrace_events();

  ASSERT_TRUE(ftrace_packets.has_cpu());
  ASSERT_EQ(ftrace_packets.cpu(), 0u);

  // Assumes order is retained.
  ASSERT_EQ(ftrace_packets.event_size(), 3);
  ASSERT_TRUE(ftrace_packets.event().at(0).has_cpu_idle());
  ASSERT_TRUE(ftrace_packets.event().at(1).has_sched_switch());
  ASSERT_TRUE(ftrace_packets.event().at(2).has_sched_waking());

  // The sched switch event's next comm should be cleared.
  const auto& sched_switch = ftrace_packets.event().at(1).sched_switch();

  ASSERT_TRUE(sched_switch.has_prev_comm());
  ASSERT_EQ(sched_switch.prev_comm(), "swapper/0");

  ASSERT_FALSE(sched_switch.has_next_comm());
}

class CommTestParams {
 public:
  CommTestParams(size_t event_index,
                 int32_t prev_pid,
                 std::optional<std::string_view> prev_comm,
                 int32_t next_pid,
                 std::optional<std::string_view> next_comm)
      : event_index_(event_index),
        prev_pid_(prev_pid),
        prev_comm_(prev_comm),
        next_pid_(next_pid),
        next_comm_(next_comm) {}

  size_t event_index() const { return event_index_; }

  int32_t prev_pid() const { return prev_pid_; }

  std::optional<std::string> prev_comm() const { return prev_comm_; }

  int32_t next_pid() const { return next_pid_; }

  std::optional<std::string> next_comm() const { return next_comm_; }

 private:
  size_t event_index_;

  int32_t prev_pid_;
  std::optional<std::string> prev_comm_;

  int32_t next_pid_;
  std::optional<std::string> next_comm_;
};

class RedactSchedSwitchTestRemoveComm
    : public RedactSchedSwitchTest,
      public testing::WithParamInterface<CommTestParams> {};

TEST_P(RedactSchedSwitchTestRemoveComm, AllEvents) {
  auto params = GetParam();

  context_.package_uid = kUidA;

  BeginBundle();

  // Cycle through all the processes: Pid A -> Pid B -> Pid C -> Pid A
  AddSwitch(kTimeA, kPidA, kCommA, kPidB, kCommB);
  AddSwitch(kTimeB, kPidB, kCommB, kPidC, kCommC);
  AddSwitch(kTimeC, kPidC, kCommC, kPidA, kCommA);

  auto packet = Transform();

  ASSERT_TRUE(packet->has_ftrace_events());

  auto& ftrace_events = packet->ftrace_events().event();

  ASSERT_EQ(ftrace_events.size(), 3u);

  auto event_index = params.event_index();

  ASSERT_TRUE(ftrace_events[event_index].has_sched_switch());

  auto& sched_switch = ftrace_events[event_index].sched_switch();

  ASSERT_EQ(sched_switch.prev_pid(), params.prev_pid());
  ASSERT_EQ(sched_switch.next_pid(), params.next_pid());

  ASSERT_EQ(sched_switch.has_prev_comm(), params.prev_comm().has_value());
  ASSERT_EQ(sched_switch.has_next_comm(), params.next_comm().has_value());

  if (sched_switch.has_prev_comm()) {
    ASSERT_EQ(sched_switch.prev_comm(), params.prev_comm());
  }

  if (sched_switch.has_next_comm()) {
    ASSERT_EQ(sched_switch.next_comm(), params.next_comm());
  }
}

// Cycle through all the processes: Pid A -> Pid B -> Pid C -> Pid A
//
// Only kPidA is attached to kUidA, so it should be the only one with a comm
// value.
INSTANTIATE_TEST_SUITE_P(
    EveryPid,
    RedactSchedSwitchTestRemoveComm,
    testing::Values(CommTestParams(0, kPidA, kCommA, kPidB, std::nullopt),
                    CommTestParams(1, kPidB, std::nullopt, kPidC, std::nullopt),
                    CommTestParams(2, kPidC, std::nullopt, kPidA, kCommA)));

}  // namespace perfetto::trace_redaction
