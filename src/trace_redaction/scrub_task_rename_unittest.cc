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

#include <string>

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/scrub_task_rename.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/trace_redaction/process_thread_timeline.h"

namespace perfetto::trace_redaction {

namespace {

// Used when a single pid is needed.
constexpr uint32_t kPid = 7971;

// Used when multiple pids are needed.
constexpr uint32_t kPidA = 7971;
constexpr uint32_t kPidB = 7145;
constexpr uint32_t kPidC = 7945;

constexpr uint64_t kUid = 27;

constexpr uint64_t kJustSomeTime = 6702094131629195;

}  // namespace

class ScrubRenameTaskTest : public testing::Test {
 protected:
  std::string packet_str_;
  protos::gen::TracePacket packet_;

  Context context_;
  ScrubTaskRename transform_;

  //  event {
  //    timestamp: 6702094131629195
  //    pid: 7971
  //    task_rename {
  //      pid: 7971
  //      oldcomm: "adbd"
  //      newcomm: "sh"
  //      oom_score_adj: -950
  //    }
  //  }
  void AddRenameEvent(uint64_t ts, uint32_t pid) {
    auto* bundle = packet_.mutable_ftrace_events();

    auto* event = bundle->add_event();
    event->set_timestamp(ts);
    event->set_pid(pid);

    auto* rename = event->mutable_task_rename();
    rename->set_pid(static_cast<int32_t>(pid));
    rename->set_oldcomm("adbd");
    rename->set_newcomm("sh");
    rename->set_oom_score_adj(-950);
  }

  //  event {
  //    timestamp: 6702094034179654
  //    pid: 7145
  //    sched_switch {
  //      prev_comm: "Job.worker 3"
  //      prev_pid: 7145
  //      prev_prio: 120
  //      prev_state: 1
  //      next_comm: "swapper/1"
  //      next_pid: 0
  //      next_prio: 120
  //    }
  //  }
  void AddAnEvent(uint64_t ts, uint32_t pid) {
    auto* bundle = packet_.mutable_ftrace_events();

    auto* event = bundle->add_event();
    event->set_timestamp(ts);
    event->set_pid(pid);

    auto* sched = event->mutable_sched_switch();
    sched->set_prev_comm("Job.worker 3");
    sched->set_prev_pid(static_cast<int32_t>(pid));
    sched->set_prev_prio(120);
    sched->set_prev_state(1);
    sched->set_next_comm("swapper/1");
    sched->set_next_pid(0);
    sched->set_next_prio(120);
  }
};

TEST_F(ScrubRenameTaskTest, ReturnErrorForNoPackage) {
  context_.timeline.reset(new ProcessThreadTimeline());
  ASSERT_FALSE(transform_.Transform(context_, &packet_str_).ok());
}

TEST_F(ScrubRenameTaskTest, ReturnErrorForNoTimeline) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  ASSERT_FALSE(transform_.Transform(context_, &packet_str_).ok());
}

TEST_F(ScrubRenameTaskTest, IgnoresNonRenamePacket) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  context_.timeline.reset(new ProcessThreadTimeline());

  AddAnEvent(kJustSomeTime, kPid);
  packet_str_ = packet_.SerializeAsString();

  auto copy = packet_str_;

  ASSERT_OK(transform_.Transform(context_, &packet_str_));
  ASSERT_EQ(copy, packet_str_);
}

// General description:
//  - One event in the trace.
//  - One event is a rename event.
//  - One event does not connect to the package.
TEST_F(ScrubRenameTaskTest, RemovesTheOnlyEvent) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  // There's no connection between the later pid (7971) and the above uid (27).
  // This means the rename packet should be dropped.
  context_.timeline.reset(new ProcessThreadTimeline());

  AddRenameEvent(kJustSomeTime, kPid);
  packet_str_ = packet_.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &packet_str_));

  protos::gen::TracePacket event_after;
  event_after.ParseFromString(packet_str_);

  // Be forgiving. If all the events were removed, there won't be a list.
  if (event_after.has_ftrace_events()) {
    ASSERT_TRUE(event_after.ftrace_events().event().empty());
  }
}

// A very simple case where given a packet with a single rename event (and
// nothing else) and is connected to the target package, the event is left
// unredacted.
TEST_F(ScrubRenameTaskTest, RetainsTheOnlyEvent) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  context_.timeline.reset(new ProcessThreadTimeline());
  context_.timeline->Append(
      ProcessThreadTimeline::Event::Open(0, kPid, 0, kUid));
  context_.timeline->Sort();

  AddRenameEvent(kJustSomeTime, kPid);
  packet_str_ = packet_.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &packet_str_));

  protos::gen::TracePacket event_after;
  event_after.ParseFromString(packet_str_);

  ASSERT_TRUE(event_after.has_ftrace_events());
  ASSERT_FALSE(event_after.ftrace_events().event().empty());
}

// When there are multiple events and multiple rename events, only the one
// rename that is not connected to package_uid will get removed.
TEST_F(ScrubRenameTaskTest, PicksOutAndRemovesRenameEvent) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  context_.timeline.reset(new ProcessThreadTimeline());
  context_.timeline->Append(
      ProcessThreadTimeline::Event::Open(0, kPidA, 0, kUid));
  context_.timeline->Sort();

  // pid A and pid B should be safe. Pid C should be removed.
  AddRenameEvent(kJustSomeTime, kPidA);
  AddAnEvent(kJustSomeTime, kPidB);
  AddRenameEvent(kJustSomeTime, kPidC);

  packet_str_ = packet_.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &packet_str_));

  protos::gen::TracePacket event_after;
  event_after.ParseFromString(packet_str_);

  ASSERT_TRUE(event_after.has_ftrace_events());
  ASSERT_EQ(event_after.ftrace_events().event_size(), 2);

  // Order should not change, so it is safe to assume that at(0) will be rename
  // and at(1) will be switch.
  //
  // For the rename task, verify the pid in addition to the type to make it
  // clear the right rename event was dropped.
  ASSERT_TRUE(event_after.ftrace_events().event().at(0).has_task_rename());
  ASSERT_EQ(event_after.ftrace_events().event().at(0).pid(), kPidA);

  ASSERT_TRUE(event_after.ftrace_events().event().at(1).has_sched_switch());
}

// Event if the event is dropped, the overall packet structure should be valid.
TEST_F(ScrubRenameTaskTest, DoesNotInvalidDatePacketStructure) {
  context_.package_name = "package name";
  context_.package_uid = kUid;

  context_.timeline.reset(new ProcessThreadTimeline());
  context_.timeline->Sort();

  // Add one or more values from the rename event all the way up to the packet.
  packet_.set_trusted_uid(9999);
  packet_.set_timestamp(kJustSomeTime);
  packet_.set_trusted_packet_sequence_id(1);

  auto* bundle = packet_.mutable_ftrace_events();
  bundle->set_cpu(1);

  AddRenameEvent(kJustSomeTime, kPid);
  packet_str_ = packet_.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &packet_str_));

  protos::gen::TracePacket event_after;
  event_after.ParseFromString(packet_str_);

  ASSERT_TRUE(event_after.has_trusted_uid());
  ASSERT_EQ(event_after.trusted_uid(), 9999);

  ASSERT_TRUE(event_after.has_timestamp());
  ASSERT_EQ(event_after.timestamp(), kJustSomeTime);

  ASSERT_TRUE(event_after.has_trusted_packet_sequence_id());
  ASSERT_EQ(event_after.trusted_packet_sequence_id(), 1u);

  ASSERT_TRUE(event_after.has_ftrace_events());

  ASSERT_TRUE(event_after.ftrace_events().has_cpu());
  ASSERT_EQ(event_after.ftrace_events().cpu(), 1u);

  // Event through everything else was found, the event was actually dropped.
  ASSERT_TRUE(event_after.ftrace_events().event().empty());
}

}  // namespace perfetto::trace_redaction
