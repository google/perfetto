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
#include "perfetto/protozero/scattered_heap_buffer.h"
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

constexpr std::string_view kCommA = "comm-a";
constexpr std::string_view kCommB = "comm-b";

}  // namespace

// Tests which nested messages and fields are removed.
class RedactSchedSwitchTest : public testing::Test {
 protected:
  void SetUp() override {
    auto* event = bundle_.add_event();

    event->set_timestamp(123456789);
    event->set_pid(kPidA);

    auto* sched_switch = event->mutable_sched_switch();
    sched_switch->set_prev_comm(std::string(kCommA));
    sched_switch->set_prev_pid(kPidA);
    sched_switch->set_next_comm(std::string(kCommB));
    sched_switch->set_next_pid(kPidB);
  }

  base::Status Redact(const Context& context,
                      protos::pbzero::FtraceEvent* event_message) {
    RedactSchedSwitch redact;

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

TEST_F(RedactSchedSwitchTest, RejectMissingPackageUid) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  auto result = Redact(context, event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactSchedSwitchTest, RejectMissingTimeline) {
  RedactSchedSwitch redact;

  Context context;
  context.package_uid = kUidA;

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  auto result = Redact(context, event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactSchedSwitchTest, ReplacePrevAndNextWithEmptyStrings) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = CreatePopulatedTimeline();

  // Neither pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidC;

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  auto result = Redact(context, event_message.get());
  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  // Cleared prev and next comm.
  ASSERT_TRUE(event.sched_switch().has_prev_comm());
  ASSERT_TRUE(event.sched_switch().prev_comm().empty());

  ASSERT_TRUE(event.sched_switch().has_next_comm());
  ASSERT_TRUE(event.sched_switch().next_comm().empty());
}

TEST_F(RedactSchedSwitchTest, ReplacePrevWithEmptyStrings) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = CreatePopulatedTimeline();

  // Only next pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidB;

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  auto result = Redact(context, event_message.get());

  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  // Only cleared the prev comm.
  ASSERT_TRUE(event.sched_switch().has_prev_comm());
  ASSERT_TRUE(event.sched_switch().prev_comm().empty());

  ASSERT_TRUE(event.sched_switch().has_next_comm());
  ASSERT_FALSE(event.sched_switch().next_comm().empty());
}

TEST_F(RedactSchedSwitchTest, ReplaceNextWithEmptyStrings) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = CreatePopulatedTimeline();

  // Only prev pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidA;

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  auto result = Redact(context, event_message.get());
  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  ASSERT_TRUE(event.sched_switch().has_prev_comm());
  ASSERT_FALSE(event.sched_switch().prev_comm().empty());

  // Only cleared the next comm.
  ASSERT_TRUE(event.sched_switch().has_next_comm());
  ASSERT_TRUE(event.sched_switch().next_comm().empty());
}

}  // namespace perfetto::trace_redaction
