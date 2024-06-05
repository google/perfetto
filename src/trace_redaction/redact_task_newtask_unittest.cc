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

#include "src/trace_redaction/redact_task_newtask.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {
constexpr uint64_t kUidA = 1;
constexpr uint64_t kUidB = 2;

constexpr int32_t kNoParent = 10;
constexpr int32_t kPidA = 11;
constexpr int32_t kPidB = 12;

constexpr std::string_view kCommA = "comm-a";

}  // namespace

// Tests which nested messages and fields are removed.
class RedactTaskNewTaskTest : public testing::Test {
 protected:
  void SetUp() override {
    auto* event = bundle_.add_event();

    event->set_timestamp(123456789);
    event->set_pid(kPidA);

    auto* new_task = event->mutable_task_newtask();
    new_task->set_comm(std::string(kCommA));
    new_task->set_pid(kPidA);
  }

  base::Status Redact(const Context& context,
                      protos::pbzero::FtraceEvent* event_message) {
    RedactTaskNewTask redact;

    auto bundle_str = bundle_.SerializeAsString();
    protos::pbzero::FtraceEventBundle::Decoder bundle_decoder(bundle_str);

    auto event_str = bundle_.event().back().SerializeAsString();
    protos::pbzero::FtraceEvent::Decoder event_decoder(event_str);

    return redact.Redact(context, bundle_decoder, event_decoder, event_message);
  }

  const std::string& event_string() const { return event_string_; }

  // This test breaks the rules for task_newtask and the timeline. The
  // timeline will report the task existing before the new task event. This
  // should not happen in the field, but it makes the test more robust.
  std::unique_ptr<ProcessThreadTimeline> CreatePopulatedTimeline() {
    auto timeline = std::make_unique<ProcessThreadTimeline>();

    timeline->Append(
        ProcessThreadTimeline::Event::Open(0, kPidA, kNoParent, kUidA));
    timeline->Append(
        ProcessThreadTimeline::Event::Open(0, kPidB, kNoParent, kUidB));
    timeline->Sort();

    return timeline;
  }

 private:
  std::string event_string_;

  std::unique_ptr<ProcessThreadTimeline> timeline_;

  protos::gen::FtraceEventBundle bundle_;
};

TEST_F(RedactTaskNewTaskTest, RejectMissingPackageUid) {
  RedactTaskNewTask redact;

  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result = Redact(context, event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactTaskNewTaskTest, RejectMissingTimeline) {
  RedactTaskNewTask redact;

  Context context;
  context.package_uid = kUidA;

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result = Redact(context, event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactTaskNewTaskTest, PidInPackageKeepsComm) {
  RedactTaskNewTask redact;

  // Because Uid A is the target, when Pid A starts (new task event), it should
  // keep its comm value.
  Context context;
  context.package_uid = kUidA;
  context.timeline = CreatePopulatedTimeline();

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result = Redact(context, event_message.get());
  ASSERT_TRUE(result.ok());

  protos::gen::FtraceEvent redacted_event;
  redacted_event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(redacted_event.has_task_newtask());
  ASSERT_TRUE(redacted_event.task_newtask().has_comm());
  ASSERT_EQ(redacted_event.task_newtask().comm(), kCommA);
}

TEST_F(RedactTaskNewTaskTest, PidOutsidePackageLosesComm) {
  RedactTaskNewTask redact;

  // Because Uid B is the target, when Pid A starts (new task event), it should
  // lose its comm value.
  Context context;
  context.package_uid = kUidB;
  context.timeline = CreatePopulatedTimeline();

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result = Redact(context, event_message.get());
  ASSERT_TRUE(result.ok());

  protos::gen::FtraceEvent redacted_event;
  redacted_event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(redacted_event.has_task_newtask());
  ASSERT_TRUE(redacted_event.task_newtask().has_comm());
  ASSERT_TRUE(redacted_event.task_newtask().comm().empty());
}

}  // namespace perfetto::trace_redaction
