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
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
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
    timeline_ = std::make_unique<ProcessThreadTimeline>();
    timeline_->Append(
        ProcessThreadTimeline::Event::Open(0, kPidA, kNoParent, kUidA));
    timeline_->Append(
        ProcessThreadTimeline::Event::Open(0, kPidB, kNoParent, kUidB));
    timeline_->Sort();

    protozero::HeapBuffered<protos::pbzero::FtraceEvent> event;
    event->set_timestamp(123456789);
    event->set_pid(kPidA);

    auto* sched_switch = event->set_sched_switch();
    sched_switch->set_prev_comm(kCommA.data(), kCommA.size());
    sched_switch->set_prev_pid(kPidA);
    sched_switch->set_next_comm(kCommB.data(), kCommB.size());
    sched_switch->set_next_pid(kPidB);

    event_string_ = event.SerializeAsString();
  }

  const std::string& event_string() const { return event_string_; }

  std::unique_ptr<ProcessThreadTimeline> timeline() {
    return std::move(timeline_);
  }

 private:
  std::string event_string_;

  std::unique_ptr<ProcessThreadTimeline> timeline_;
};

TEST_F(RedactSchedSwitchTest, RejectMissingPackageUid) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result =
      redact.Redact(context, event_decoder, event_decoder.sched_switch(),
                    event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactSchedSwitchTest, RejectMissingTimeline) {
  RedactSchedSwitch redact;

  Context context;
  context.package_uid = kUidA;

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result =
      redact.Redact(context, event_decoder, event_decoder.sched_switch(),
                    event_message.get());
  ASSERT_FALSE(result.ok());
}

TEST_F(RedactSchedSwitchTest, ClearsPrevAndNext) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = timeline();

  // Neither pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidC;

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result =
      redact.Redact(context, event_decoder, event_decoder.sched_switch(),
                    event_message.get());
  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  // Pid should always carry over; only the comm value should get removed.
  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_FALSE(event.sched_switch().has_next_comm());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_FALSE(event.sched_switch().has_prev_comm());
}

TEST_F(RedactSchedSwitchTest, ClearsPrev) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = timeline();

  // Only next pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidB;

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result =
      redact.Redact(context, event_decoder, event_decoder.sched_switch(),
                    event_message.get());
  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  // Pid should always carry over; only the comm value should get removed.
  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_TRUE(event.sched_switch().has_next_comm());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_FALSE(event.sched_switch().has_prev_comm());
}

TEST_F(RedactSchedSwitchTest, ClearNext) {
  RedactSchedSwitch redact;

  Context context;
  context.timeline = timeline();

  // Only prev pid is connected to the target package (see timeline
  // initialization).
  context.package_uid = kUidA;

  protos::pbzero::FtraceEvent::Decoder event_decoder(event_string());
  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  auto result =
      redact.Redact(context, event_decoder, event_decoder.sched_switch(),
                    event_message.get());
  ASSERT_OK(result) << result.c_message();

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  // Pid should always carry over; only the comm value should get removed.
  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_FALSE(event.sched_switch().has_next_comm());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_TRUE(event.sched_switch().has_prev_comm());
}

}  // namespace perfetto::trace_redaction
