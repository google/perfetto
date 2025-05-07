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

#include "src/trace_redaction/redact_process_events.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/power.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {
constexpr uint64_t kCpu = 1;

constexpr uint64_t kUidA = 1;
constexpr uint64_t kUidB = 2;

constexpr int32_t kNoParent = 10;
constexpr int32_t kPidA = 11;
constexpr int32_t kPidB = 12;

// Used as a child of kPidA.
constexpr int32_t kPidAA = kPidA * 10;

constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = 1000;

constexpr std::string_view kCommA = "comm-a";
constexpr std::string_view kCommB = "comm-b";
}  // namespace

class RedactProcessEventsTest : public testing::Test {
 protected:
  void SetUp() {
    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
};

TEST_F(RedactProcessEventsTest, RejectMissingPackageUid) {
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protos::gen::TracePacket packet;
  auto packet_str = packet.SerializeAsString();

  ASSERT_FALSE(redact_.Transform(context, &packet_str).ok());
}

TEST_F(RedactProcessEventsTest, RejectMissingTimeline) {
  Context context;
  context.package_uid = kUidA;

  protos::gen::TracePacket packet;
  auto packet_str = packet.SerializeAsString();

  ASSERT_FALSE(redact_.Transform(context, &packet_str).ok());
}

TEST_F(RedactProcessEventsTest, RejectMissingPacket) {
  Context context;
  context.package_uid = kUidA;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  ASSERT_FALSE(redact_.Transform(context, nullptr).ok());
}

TEST_F(RedactProcessEventsTest, EmptyMissingPacket) {
  Context context;
  context.package_uid = kUidA;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  std::string packet_str;

  ASSERT_FALSE(redact_.Transform(context, &packet_str).ok());
}

// Tests which nested messages and fields are removed.
class RedactNewTaskTest : public testing::Test {
 protected:
  void SetUp() override {
    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    auto* event = events->add_event();
    event->set_timestamp(kTimeB);
    event->set_pid(kPidA);

    auto* new_task = event->mutable_task_newtask();
    new_task->set_clone_flags(0);
    new_task->set_comm(std::string(kCommA));
    new_task->set_oom_score_adj(0);
    new_task->set_pid(kPidA);

    // This test breaks the rules for task_newtask and the timeline. The
    // timeline will report the task existing before the new task event. This
    // should not happen in the field, but it makes the test more robust.

    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
  protos::gen::TracePacket packet_;
  Context context_;
};

TEST_F(RedactNewTaskTest, KeepCommInPackage) {
  redact_.emplace_modifier<ClearComms>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep new
  // task.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_task_newtask());

  const auto& new_task = event.task_newtask();

  ASSERT_TRUE(new_task.has_pid());
  ASSERT_EQ(new_task.pid(), kPidA);

  ASSERT_TRUE(new_task.has_comm());
  ASSERT_EQ(new_task.comm(), kCommA);
}

TEST_F(RedactNewTaskTest, ClearCommOutsidePackage) {
  redact_.emplace_modifier<ClearComms>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid B; clear the
  // comm.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_task_newtask());

  const auto& new_task = event.task_newtask();

  ASSERT_TRUE(new_task.has_pid());
  ASSERT_EQ(new_task.pid(), kPidA);

  ASSERT_TRUE(new_task.has_comm());
  ASSERT_TRUE(new_task.comm().empty());
}

TEST_F(RedactNewTaskTest, KeepTaskInPackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep new
  // task.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_task_newtask());

  const auto& new_task = event.task_newtask();

  ASSERT_TRUE(new_task.has_pid());
  ASSERT_EQ(new_task.pid(), kPidA);
}

TEST_F(RedactNewTaskTest, DropTaskOutsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid B; drop new
  // task event.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // The task should have been removed, but the event will still remain.
  ASSERT_FALSE(packet.ftrace_events().event().at(0).has_task_newtask());
}

class RedactProcessFree : public testing::Test {
 protected:
  void SetUp() override {
    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    auto* event = events->add_event();
    event->set_timestamp(kTimeB);
    event->set_pid(kPidA);

    auto* process_free = event->mutable_sched_process_free();
    process_free->set_comm(std::string(kCommA));
    process_free->set_pid(kPidA);
    process_free->set_prio(0);

    // By default, this timeline is invalid. sched_process_free would insert
    // close events. If sched_process_free appended at time B a close event
    // would be created at time B.
    //
    // Timeline spans are inclusive-start but exclusive-end, so a
    // sched_process_free will never pass a "connected to package" test. The
    // timeline is created to make testing easier.
    //
    // If a test wants a "valid" timeline, it should add a close event at
    // sched_process_free.

    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
  protos::gen::TracePacket packet_;
  Context context_;
};

TEST_F(RedactProcessFree, KeepsCommInPackage) {
  redact_.emplace_modifier<ClearComms>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep comm.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_sched_process_free());

  const auto& process_free = event.sched_process_free();

  ASSERT_TRUE(process_free.has_pid());
  ASSERT_EQ(process_free.pid(), kPidA);

  ASSERT_TRUE(process_free.has_comm());
  ASSERT_EQ(process_free.comm(), kCommA);
}

TEST_F(RedactProcessFree, DropsCommOutsidePackage) {
  redact_.emplace_modifier<ClearComms>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid B; drop comm.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_sched_process_free());

  const auto& process_free = event.sched_process_free();

  ASSERT_TRUE(process_free.has_pid());
  ASSERT_EQ(process_free.pid(), kPidA);

  ASSERT_TRUE(process_free.has_comm());
  ASSERT_TRUE(process_free.comm().empty());
}

TEST_F(RedactProcessFree, KeepsCommAtProcessFree) {
  redact_.emplace_modifier<ClearComms>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid A; Process free
  // marks the end of Pid A, but process free is inclusive and Pid A is only
  // free after the event.
  context_.package_uid = kUidA;

  context_.timeline->Append(ProcessThreadTimeline::Event::Close(kTimeB, kPidA));
  context_.timeline->Sort();

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_sched_process_free());

  const auto& process_free = event.sched_process_free();

  ASSERT_TRUE(process_free.has_pid());
  ASSERT_EQ(process_free.pid(), kPidA);

  ASSERT_TRUE(process_free.has_comm());
  ASSERT_EQ(process_free.comm(), kCommA);
}

TEST_F(RedactProcessFree, KeepTaskInPackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep new
  // task.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_sched_process_free());

  const auto& process_free = event.sched_process_free();

  ASSERT_TRUE(process_free.has_pid());
  ASSERT_EQ(process_free.pid(), kPidA);
}

TEST_F(RedactProcessFree, DropTaskOutsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The new task is for Pid A. Pid A is part of Uid A. Keep Uid B; drop new
  // task event.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // The task should have been removed, but the event will still remain.
  ASSERT_FALSE(packet.ftrace_events().event().at(0).has_sched_process_free());
}

// There were two places where PID vales can appear:
//
//    1. At the ftrace event - the PID must appear here. If the PID does not
//       appear here, the packet is invalid.
//
//    2. At the rename task - if the PID appeared here, it would match the
//       ftrace event's PID. The pid at the task level was removed.
//
// Because the PID was removed at the task-level, the primitive should not
// depend on being or not being present. To test this, we test every
// permutation.
class RedactRenamePidsTest : public testing::Test {
 protected:
  void SetUp() {
    redact_.emplace_filter<AllowAll>();
    redact_.emplace_modifier<ClearComms>();

    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    // A package uid must be provided.
    context_.package_uid = kPidA;

    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    event_ = events->add_event();
    event_->set_timestamp(kTimeA);

    rename_task_ = event_->mutable_task_rename();
    rename_task_->set_newcomm(std::string(kCommB));
    rename_task_->set_oldcomm(std::string(kCommA));
    rename_task_->set_oom_score_adj(0);
  }

  protos::gen::TracePacket packet_;
  protos::gen::FtraceEvent* event_;
  protos::gen::TaskRenameFtraceEvent* rename_task_;

  RedactProcessEvents redact_;
  Context context_;
};

TEST_F(RedactRenamePidsTest, PidInEventAndTask) {
  event_->set_pid(kPidA);
  rename_task_->set_pid(kPidA);

  auto packet_string = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_string));
}

TEST_F(RedactRenamePidsTest, PidInEventButNotTask) {
  event_->set_pid(kPidA);

  auto packet_string = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_string));
}

TEST_F(RedactRenamePidsTest, PidNotInEventButInTask) {
  rename_task_->set_pid(kPidA);

  auto packet_string = packet_.SerializeAsString();
  ASSERT_FALSE(redact_.Transform(context_, &packet_string).ok());
}

TEST_F(RedactRenamePidsTest, PidsNotInEventAndNotInTask) {
  auto packet_string = packet_.SerializeAsString();
  ASSERT_FALSE(redact_.Transform(context_, &packet_string).ok());
}

// If a PID was added at the event and the task level, only the task level PID
// should persist.
TEST_F(RedactRenamePidsTest, DropPidFromTask) {
  event_->set_pid(kPidA);
  rename_task_->set_pid(kPidA);

  auto packet_string = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_string));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_string));

  // The task should still exist, but the pid should not remain.
  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event_size(), 1);
  ASSERT_TRUE(packet.ftrace_events().event().front().has_task_rename());
  ASSERT_FALSE(packet.ftrace_events().event().front().task_rename().has_pid());
}

TEST_F(RedactRenamePidsTest, PidInTaskOverridesPidInEvent) {
  // The only allowed pid will be the pid on the task. If it was not set, it
  // would have been removed. But because the rename task overrides the ftrace
  // event's pid, it should be retained.
  redact_.emplace_filter(std::make_unique<MatchesPid>(kPidA));

  context_.package_uid = kUidA;
  event_->set_pid(kPidB);
  rename_task_->set_pid(kPidA);

  auto packet_string = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_string));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_string));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event_size(), 1);
  ASSERT_TRUE(packet.ftrace_events().event().front().has_task_rename());
}

// Redact comm values
//
// The comm values are the process names. "Old comm" is the previous name and
// the "new comm" is the new name. Rename events should only carry over when
// there is a pid and the pid belongs to the target package.
class RedactCommValuesTest : public testing::Test {
 protected:
  void SetUp() {
    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    redact_.emplace_filter<ConnectedToPackage>();
    redact_.emplace_modifier<ClearComms>();

    events_ = packet_.mutable_ftrace_events();

    event_ = events_->add_event();
    event_->set_timestamp(kTimeA);

    rename_ = event_->mutable_task_rename();
    rename_->set_newcomm(std::string(kCommB));
    rename_->set_oldcomm(std::string(kCommA));
    rename_->set_oom_score_adj(0);
  }

  protos::gen::TracePacket packet_;
  protos::gen::FtraceEventBundle* events_;
  protos::gen::FtraceEvent* event_;
  protos::gen::TaskRenameFtraceEvent* rename_;

  RedactProcessEvents redact_;
  Context context_;
};

// The UID UID_A has a PID PID_A which has one rename task. If the target UID
// is UID_A, then PID_A is included, which means that rename task will be
// included.
TEST_F(RedactCommValuesTest, KeepCommInsideOfPackage) {
  context_.package_uid = kUidA;

  event_->set_pid(kPidA);

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_task_rename());

  const auto& task_rename = event.task_rename();

  ASSERT_FALSE(task_rename.has_pid());
  ASSERT_TRUE(task_rename.has_oldcomm());
  ASSERT_TRUE(task_rename.has_newcomm());

  ASSERT_EQ(task_rename.oldcomm(), kCommA);
  ASSERT_EQ(task_rename.newcomm(), kCommB);
}

// If the target UID is UID_B. Then PID_A, which contains the rename task, would
// not be included in UID_B's tree, and therefore dropped.
TEST_F(RedactCommValuesTest, DropCommOutsideOfPackage) {
  context_.package_uid = kUidB;

  event_->set_pid(kPidA);

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_FALSE(event.has_task_rename());
}

TEST_F(RedactCommValuesTest, FailsWhenThereIsNoPidOnTheEvent) {
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_FALSE(redact_.Transform(context_, &packet_str).ok());
}

class RedactRenameTest : public testing::Test {
 protected:
  void SetUp() {
    redact_.emplace_filter<ConnectedToPackage>();
    redact_.emplace_modifier<ClearComms>();

    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    auto* event = packet_.mutable_ftrace_events()->add_event();
    event->set_timestamp(kTimeB);
    event->set_pid(kPidA);

    auto* rename = event->mutable_task_rename();
    rename->set_newcomm(std::string(kCommB));
    rename->set_oldcomm(std::string(kCommA));
    rename->set_oom_score_adj(0);
  }

  protos::gen::TracePacket packet_;
  RedactProcessEvents redact_;
  Context context_;
};

TEST_F(RedactRenameTest, KeepTaskInsidePackage) {
  // The rename task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep
  // comm.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_task_rename());
}

TEST_F(RedactRenameTest, DropTaskOutsidePackage) {
  // The rename task is for Pid A. Pid A is part of Uid A. Keep Uid B; drop
  // task.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // The task should have been removed, but the event will still remain.
  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_FALSE(event.has_task_rename());
}

class RedactPrintTest : public testing::Test {
 protected:
  void SetUp() {
    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    auto* event = events->add_event();
    event->set_timestamp(kTimeB);
    event->set_pid(kPidA);

    // The rename event pid will match the ftrace event pid.
    auto* print = event->mutable_print();
    print->set_buf(std::string(kCommA));
    print->set_ip(0);

    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
  protos::gen::TracePacket packet_;
  Context context_;
};

TEST_F(RedactPrintTest, KeepTaskInsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The rename task is for Pid A. Pid A is part of Uid A. Keep Uid A; keep
  // comm.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_print());
}

TEST_F(RedactPrintTest, DropTaskOutsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The rename task is for Pid A. Pid A is part of Uid A. Keep Uid B; drop
  // task.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // The task should have been removed, but the event will still remain.
  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_FALSE(event.has_print());
}

class RedactSuspendResumeTest : public testing::Test {
 protected:
  void SetUp() {
    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    for (const auto& action : {"syscore_suspend", "syscore_resume",
                               "timekeeping_freeze", "not-allowed"}) {
      auto* event = events->add_event();
      event->set_timestamp(kTimeB);
      event->set_pid(kPidA);

      auto* suspend_resume = event->mutable_suspend_resume();
      suspend_resume->set_action(action);
      suspend_resume->set_start(0);
      suspend_resume->set_val(3);
    }

    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));
    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));
    context_.timeline->Sort();

    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
  protos::gen::TracePacket packet_;
  Context context_;
};

TEST_F(RedactSuspendResumeTest, KeepTaskInsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 4u);
}

// Only actions in the allowlist should be allowed.
//
// TODO(vaage): The allowlist is not configurable right now. If moved into the
// context, it could be configured.
TEST_F(RedactSuspendResumeTest, FiltersByAllowlist) {
  redact_.emplace_filter<ConnectedToPackage>();

  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 4u);

  {
    const auto& event = packet.ftrace_events().event().at(0);
    ASSERT_TRUE(event.has_suspend_resume());
    ASSERT_EQ(event.suspend_resume().action(), "syscore_suspend");
  }

  {
    const auto& event = packet.ftrace_events().event().at(1);
    ASSERT_TRUE(event.has_suspend_resume());
    ASSERT_EQ(event.suspend_resume().action(), "syscore_resume");
  }

  {
    const auto& event = packet.ftrace_events().event().at(2);
    ASSERT_TRUE(event.has_suspend_resume());
    ASSERT_EQ(event.suspend_resume().action(), "timekeeping_freeze");
  }

  // The fourth entry is an invalid action. While the other entries are valid
  // and are retained, this one should be dropped.
  ASSERT_FALSE(packet.ftrace_events().event().at(3).has_suspend_resume());
}

TEST_F(RedactSuspendResumeTest, DropTaskOutsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 4u);

  // The task should have been removed, but the event will still remain.
  ASSERT_FALSE(packet.ftrace_events().event().at(0).has_suspend_resume());
  ASSERT_FALSE(packet.ftrace_events().event().at(1).has_suspend_resume());
  ASSERT_FALSE(packet.ftrace_events().event().at(2).has_suspend_resume());
  ASSERT_FALSE(packet.ftrace_events().event().at(3).has_suspend_resume());
}

class RedactSchedBlockReasonTest : public testing::Test {
 protected:
  void SetUp() {
    auto* events = packet_.mutable_ftrace_events();
    events->set_cpu(kCpu);

    {
      auto* event = events->add_event();
      event->set_timestamp(kTimeB);
      event->set_pid(kPidB);

      auto* reason = event->mutable_sched_blocked_reason();
      reason->set_caller(3);
      reason->set_io_wait(7);
      reason->set_pid(kPidAA);
    }

    context_.timeline = std::make_unique<ProcessThreadTimeline>();

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidA, kNoParent, kUidA));

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidAA, kPidA));

    context_.timeline->Append(
        ProcessThreadTimeline::Event::Open(kTimeA, kPidB, kNoParent, kUidB));

    context_.timeline->Sort();

    redact_.emplace_modifier<DoNothing>();
    redact_.emplace_filter<AllowAll>();
  }

  RedactProcessEvents redact_;
  protos::gen::TracePacket packet_;
  Context context_;
};

// Implementation detail: No events are removed, only inner messages.
TEST_F(RedactSchedBlockReasonTest, KeepTaskInsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The blocking events target kPidA is connected to kUidA. Since the target is
  // kUidA, the blocking events should be retained.
  context_.package_uid = kUidA;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // Assumption, event order does not change. The first event was connected to
  // kUidA.
  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_TRUE(event.has_sched_blocked_reason());
  ASSERT_EQ(event.sched_blocked_reason().pid(), kPidAA);
}

// Implementation detail: No events are removed, only inner messages.
TEST_F(RedactSchedBlockReasonTest, DropTaskOutsidePackage) {
  redact_.emplace_filter<ConnectedToPackage>();

  // The blocking events target kPidA is connected to kUidA. Since the target is
  // kUidB, the blocking events should be dropped.
  context_.package_uid = kUidB;

  auto packet_str = packet_.SerializeAsString();
  ASSERT_OK(redact_.Transform(context_, &packet_str));

  protos::gen::TracePacket packet;
  ASSERT_TRUE(packet.ParseFromString(packet_str));

  ASSERT_TRUE(packet.has_ftrace_events());
  ASSERT_EQ(packet.ftrace_events().event().size(), 1u);

  // Assumption, event order does not change. The first event was connected to
  // kUidA.
  const auto& event = packet.ftrace_events().event().at(0);
  ASSERT_FALSE(event.has_sched_blocked_reason());
}

}  // namespace perfetto::trace_redaction
