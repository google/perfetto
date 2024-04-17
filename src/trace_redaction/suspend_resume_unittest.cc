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

#include "src/trace_redaction/suspend_resume.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.gen.h"

namespace perfetto::trace_redaction {

TEST(AllowSuspendResumeTest, UpdatesTracePacketAllowlist) {
  Context context;

  // Start with a non-empty allow-list item.
  context.ftrace_packet_allow_list.insert(
      protos::pbzero::FtraceEvent::kPrintFieldNumber);

  ASSERT_EQ(context.ftrace_packet_allow_list.size(), 1u);

  AllowSuspendResume allow;
  auto status = allow.Build(&context);
  ASSERT_OK(status) << status.message();

  // Print should still be present. The allowlist should have been updated, not
  // replaced.
  ASSERT_EQ(context.ftrace_packet_allow_list.count(
                protos::pbzero::FtraceEvent::kPrintFieldNumber),
            1u);

  ASSERT_EQ(context.ftrace_packet_allow_list.count(
                protos::pbzero::FtraceEvent::kSuspendResumeFieldNumber),
            1u);
}

TEST(AllowSuspendResumeTest, UpdatesSuspendResumeAllowlist) {
  Context context;

  ASSERT_TRUE(context.suspend_result_allow_list.empty());

  AllowSuspendResume allow;
  auto status = allow.Build(&context);
  ASSERT_OK(status) << status.message();

  ASSERT_FALSE(context.suspend_result_allow_list.empty());
}

class SuspendResumeTest : public testing::Test {
 protected:
  void SetUp() {
    AllowSuspendResume allow;
    ASSERT_OK(allow.Build(&context_));
  }

  protos::gen::FtraceEvent CreateSuspendResumeEvent(
      const std::string* action) const {
    protos::gen::FtraceEvent event;
    event.set_timestamp(1234);
    event.set_pid(0);

    auto* suspend_resume = event.mutable_suspend_resume();

    if (action) {
      suspend_resume->set_action(*action);
    }

    return event;
  }

  protos::gen::FtraceEvent CreateOtherEvent() const {
    protos::gen::FtraceEvent event;
    event.set_timestamp(1234);
    event.set_pid(0);

    auto* print = event.mutable_print();
    print->set_buf("This is a message");

    return event;
  }

  const Context& context() const { return context_; }

 private:
  Context context_;
};

// The suspend-resume filter is not responsible for non-suspend-resume events.
// It should assume that another filter will handle it and it should just allow
// those events through
TEST_F(SuspendResumeTest, AcceptsOtherEvents) {
  auto event = CreateOtherEvent();
  auto event_array = event.SerializeAsArray();
  protozero::ConstBytes event_bytes{event_array.data(), event_array.size()};

  FilterSuspendResume filter;
  ASSERT_TRUE(filter.KeepEvent(context(), event_bytes));
}

TEST_F(SuspendResumeTest, AcceptsEventsWithNoName) {
  auto event = CreateSuspendResumeEvent(nullptr);
  auto event_array = event.SerializeAsArray();
  protozero::ConstBytes event_bytes{event_array.data(), event_array.size()};

  Context context;

  FilterSuspendResume filter;
  ASSERT_TRUE(filter.KeepEvent(context, event_bytes));
}

TEST_F(SuspendResumeTest, AcceptsEventsWithValidName) {
  // This value is from "src/trace_redaction/suspend_resume.cc".
  std::string name = "syscore_suspend";

  auto event = CreateSuspendResumeEvent(&name);
  auto event_array = event.SerializeAsArray();
  protozero::ConstBytes event_bytes{event_array.data(), event_array.size()};

  FilterSuspendResume filter;
  ASSERT_TRUE(filter.KeepEvent(context(), event_bytes));
}

TEST_F(SuspendResumeTest, RejectsEventsWithInvalidName) {
  std::string name = "hello world";

  auto event = CreateSuspendResumeEvent(&name);
  auto event_array = event.SerializeAsArray();
  protozero::ConstBytes event_bytes{event_array.data(), event_array.size()};

  FilterSuspendResume filter;
  ASSERT_FALSE(filter.KeepEvent(context(), event_bytes));
}

}  // namespace perfetto::trace_redaction
