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

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/redact_ftrace_events.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {
namespace {
constexpr uint64_t kTime = 10;

constexpr int32_t kPidA = 7;
constexpr int32_t kPidB = 8;
constexpr int32_t kPidC = 9;

constexpr int32_t kCpu = 3;
}  // namespace

// Tests which nested messages and fields are removed.
class FilterFtraceUsingAllowlistTest : public testing::Test {
 protected:
  void SetUp() override {
    transform_.emplace_ftrace_filter<FilterFtracesUsingAllowlist>();
    transform_.emplace_post_filter_modifier<DoNothing>();

    // RedactFtraceEvents expects ftrace events bundles to have a cpu value.
    // These tests don't rely on one, so set it here and forget about it.
    packet_.mutable_ftrace_events()->set_cpu(kCpu);
  }

  // task_rename should be in the allow-list.
  static void AddTaskRename(protos::gen::FtraceEventBundle* bundle,
                            int32_t pid,
                            const std::string& old_comm,
                            const std::string& new_comm) {
    auto* e = bundle->add_event();
    e->set_pid(static_cast<uint32_t>(pid));
    e->set_timestamp(kTime);
    e->mutable_task_rename()->set_pid(pid);
    e->mutable_task_rename()->set_oldcomm(old_comm);
    e->mutable_task_rename()->set_newcomm(new_comm);
  }

  static void AddClockSetRate(protos::gen::FtraceEventBundle* bundle,
                              int32_t pid,
                              uint64_t cpu,
                              const std::string& name,
                              uint64_t state) {
    auto* e = bundle->add_event();
    e->set_pid(static_cast<uint32_t>(pid));
    e->set_timestamp(kTime);
    e->mutable_clock_set_rate()->set_cpu_id(cpu);
    e->mutable_clock_set_rate()->set_name(name);
    e->mutable_clock_set_rate()->set_state(state);
  }

  RedactFtraceEvents transform_;

  protos::gen::TracePacket packet_;
};

TEST_F(FilterFtraceUsingAllowlistTest, ReturnErrorForNullPacket) {
  Context context;
  ASSERT_FALSE(transform_.Transform(context, nullptr).ok());
}

TEST_F(FilterFtraceUsingAllowlistTest, ReturnErrorForEmptyPacket) {
  std::string packet_str = "";

  Context context;
  ASSERT_FALSE(transform_.Transform(context, &packet_str).ok());
}

TEST_F(FilterFtraceUsingAllowlistTest, IgnorePacketWithNoFtraceEvents) {
  auto* tree = packet_.mutable_process_tree();

  auto& process = tree->mutable_processes()->emplace_back();
  process.set_pid(1);
  process.set_ppid(2);
  process.set_uid(3);

  auto& thread = tree->mutable_threads()->emplace_back();
  thread.set_name("hello world");
  thread.set_tgid(1);
  thread.set_tid(135);

  auto original_packet = packet_.SerializeAsString();
  auto packet = original_packet;

  Context context;
  context.ftrace_packet_allow_list = {
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber};

  auto transform_status = transform_.Transform(context, &packet);
  ASSERT_OK(transform_status) << transform_status.c_message();

  // The packet doesn't have any ftrace events. It should not be affected by
  // this transform.
  ASSERT_EQ(original_packet, packet);
}

// There are some values in a ftrace event that sits behind the ftrace bundle.
// These values should be retained.
TEST_F(FilterFtraceUsingAllowlistTest, KeepsFtraceBundleSiblingValues) {
  auto* ftrace_events = packet_.mutable_ftrace_events();

  ftrace_events->set_cpu(kCpu);
  AddTaskRename(ftrace_events, kPidA, "old_comm", "new_comm_7");
  AddClockSetRate(ftrace_events, kPidA, kCpu, "cool cpu name", 1);

  auto original_packet = packet_.SerializeAsString();
  auto packet = original_packet;

  Context context;
  context.ftrace_packet_allow_list = {
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber};

  ASSERT_OK(transform_.Transform(context, &packet));

  protos::gen::TracePacket gen_packet;
  gen_packet.ParseFromString(packet);

  ASSERT_TRUE(gen_packet.has_ftrace_events());
  const auto& gen_events = gen_packet.ftrace_events();

  // Because the CPU sits beside the event list, and not inside the event list,
  // the CPU value should be retained.
  ASSERT_TRUE(gen_events.has_cpu());
  ASSERT_EQ(gen_events.cpu(), static_cast<uint32_t>(kCpu));

  // ClockSetRate should be dropped. Only TaskRename should remain.
  ASSERT_EQ(gen_events.event_size(), 1);
  ASSERT_FALSE(gen_events.event().front().has_clock_set_rate());
  ASSERT_TRUE(gen_events.event().front().has_task_rename());
}

TEST_F(FilterFtraceUsingAllowlistTest, KeepsAllowedEvents) {
  Context context;
  context.ftrace_packet_allow_list = {
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber,
  };

  AddTaskRename(packet_.mutable_ftrace_events(), kPidA, "old_comm",
                "new_comm_7");
  AddTaskRename(packet_.mutable_ftrace_events(), kPidB, "old_comm",
                "new_comm_8");
  AddTaskRename(packet_.mutable_ftrace_events(), kPidC, "old_comm",
                "new_comm_9");

  auto before_str = packet_.SerializeAsString();
  auto after_str = before_str;

  ASSERT_OK(transform_.Transform(context, &after_str));

  protos::gen::TracePacket after;
  after.ParseFromString(after_str);

  // Implementation detail: ScrubFtraceEvents may change entry order. The diff
  // must be order independent. Sort the events by pid, this will make it easier
  // to assert values.
  auto events = after.ftrace_events().event();
  std::sort(events.begin(), events.end(),
            [](const auto& l, const auto& r) { return l.pid() < r.pid(); });

  ASSERT_EQ(events.size(), 3u);

  ASSERT_TRUE(events[0].has_task_rename());
  ASSERT_EQ(events[0].task_rename().pid(), kPidA);
  ASSERT_EQ(events[0].task_rename().oldcomm(), "old_comm");
  ASSERT_EQ(events[0].task_rename().newcomm(), "new_comm_7");

  ASSERT_TRUE(events[1].has_task_rename());
  ASSERT_EQ(events[1].task_rename().pid(), kPidB);
  ASSERT_EQ(events[1].task_rename().oldcomm(), "old_comm");
  ASSERT_EQ(events[1].task_rename().newcomm(), "new_comm_8");

  ASSERT_TRUE(events[2].has_task_rename());
  ASSERT_EQ(events[2].task_rename().pid(), kPidC);
  ASSERT_EQ(events[2].task_rename().oldcomm(), "old_comm");
  ASSERT_EQ(events[2].task_rename().newcomm(), "new_comm_9");
}

// Only the specific non-allowed events should be removed from the event list.
TEST_F(FilterFtraceUsingAllowlistTest, OnlyDropsNotAllowedEvents) {
  // AddTaskRename >> Keep
  // AddClockSetRate >> Drop
  AddTaskRename(packet_.mutable_ftrace_events(), kPidA, "old_comm",
                "new_comm_7");
  AddClockSetRate(packet_.mutable_ftrace_events(), kPidA, kCpu, "cool cpu name",
                  1);
  AddTaskRename(packet_.mutable_ftrace_events(), kPidB, "old_comm",
                "new_comm_8");
  AddTaskRename(packet_.mutable_ftrace_events(), kPidC, "old_comm",
                "new_comm_9");
  auto packet = packet_.SerializeAsString();

  Context context;
  context.ftrace_packet_allow_list = {
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber};

  ASSERT_OK(transform_.Transform(context, &packet));

  protos::gen::TracePacket modified_packet;
  ASSERT_TRUE(modified_packet.ParseFromString(packet));

  // Only the clock set rate event should have been removed (drop 1 of the 4
  // events).
  ASSERT_TRUE(modified_packet.has_ftrace_events());
  ASSERT_EQ(modified_packet.ftrace_events().event_size(), 3);

  // All ftrace events should be rename events.
  const auto& events = modified_packet.ftrace_events().event();

  ASSERT_TRUE(events.at(0).has_task_rename());
  ASSERT_EQ(events.at(0).task_rename().pid(), kPidA);

  ASSERT_TRUE(events.at(1).has_task_rename());
  ASSERT_EQ(events.at(1).task_rename().pid(), kPidB);

  ASSERT_TRUE(events.at(2).has_task_rename());
  ASSERT_EQ(events.at(2).task_rename().pid(), kPidC);
}

}  // namespace perfetto::trace_redaction
