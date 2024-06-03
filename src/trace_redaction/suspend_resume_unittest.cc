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

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.gen.h"
#include "src/trace_redaction/redact_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"

namespace perfetto::trace_redaction {

class SuspendResumeTest : public testing::Test {
 protected:
  protos::gen::FtraceEventBundle CreateSuspendResumeEvent(
      const std::string* action) const {
    protos::gen::FtraceEventBundle bundle;

    auto* event = bundle.add_event();
    event->set_timestamp(1234);
    event->set_pid(0);

    auto* suspend_resume = event->mutable_suspend_resume();

    if (action) {
      suspend_resume->set_action(*action);
    }

    return bundle;
  }

  protos::gen::FtraceEventBundle CreateOtherEvent() const {
    protos::gen::FtraceEventBundle bundle;

    auto* event = bundle.add_event();
    event->set_timestamp(1234);
    event->set_pid(0);

    auto* print = event->mutable_print();
    print->set_buf("This is a message");

    return bundle;
  }

  const Context& context() const { return context_; }

 private:
  Context context_;
};

// The suspend-resume filter is not responsible for non-suspend-resume events.
// It should assume that another filter will handle it and it should just allow
// those events through
TEST_F(SuspendResumeTest, AcceptsOtherEvents) {
  auto bundle = CreateOtherEvent();
  auto bundle_str = bundle.SerializeAsString();

  protozero::ProtoDecoder decoder(bundle_str);

  auto event =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kEventFieldNumber);
  ASSERT_TRUE(event.valid());

  FilterFtraceUsingSuspendResume filter;
  ASSERT_TRUE(filter.Includes(context(), event));
}

TEST_F(SuspendResumeTest, AcceptsEventsWithNoName) {
  auto bundle = CreateSuspendResumeEvent(nullptr);
  auto bundle_str = bundle.SerializeAsString();

  protozero::ProtoDecoder decoder(bundle_str);

  auto event =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kEventFieldNumber);
  ASSERT_TRUE(event.valid());

  FilterFtraceUsingSuspendResume filter;
  ASSERT_TRUE(filter.Includes(context(), event));
}

TEST_F(SuspendResumeTest, AcceptsEventsWithValidName) {
  // This value is from "src/trace_redaction/suspend_resume.cc".
  std::string name = "syscore_suspend";

  auto bundle = CreateSuspendResumeEvent(&name);
  auto bundle_str = bundle.SerializeAsString();

  protozero::ProtoDecoder decoder(bundle_str);

  auto event =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kEventFieldNumber);
  ASSERT_TRUE(event.valid());

  FilterFtraceUsingSuspendResume filter;
  ASSERT_TRUE(filter.Includes(context(), event));
}

TEST_F(SuspendResumeTest, RejectsEventsWithInvalidName) {
  std::string name = "hello world";

  auto bundle = CreateSuspendResumeEvent(&name);
  auto bundle_str = bundle.SerializeAsString();

  protozero::ProtoDecoder decoder(bundle_str);

  auto event =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kEventFieldNumber);
  ASSERT_TRUE(event.valid());

  FilterFtraceUsingSuspendResume filter;
  ASSERT_FALSE(filter.Includes(context(), event));
}

}  // namespace perfetto::trace_redaction
