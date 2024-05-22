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

#include "src/trace_redaction/remove_process_free_comm.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

class RemoveProcessFreeCommTest : public testing::Test {
 protected:
  void SetUp() override {
    auto* source_event = bundle.add_event();
    source_event->set_timestamp(123456789);
    source_event->set_pid(10);

    process_free = source_event->mutable_sched_process_free();
  }

  base::Status Redact(protos::pbzero::FtraceEvent* event_message) {
    RemoveProcessFreeComm redact;
    Context context;

    auto bundle_str = bundle.SerializeAsString();
    protos::pbzero::FtraceEventBundle::Decoder bundle_decoder(bundle_str);

    auto event_str = bundle.event().back().SerializeAsString();
    protos::pbzero::FtraceEvent::Decoder event_decoder(event_str);

    return redact.Redact(context, bundle_decoder, event_decoder, event_message);
  }

  protos::gen::FtraceEventBundle bundle;
  protos::gen::SchedProcessFreeFtraceEvent* process_free;
};

// A free event will always test as "not active". So the comm value should
// always be replaced with an empty string.
TEST_F(RemoveProcessFreeCommTest, ClearsCommValue) {
  process_free->set_comm("comm-a");
  process_free->set_pid(11);
  process_free->set_prio(0);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;

  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent redacted_event;
  redacted_event.ParseFromString(event_message.SerializeAsString());

  // No process free event should have been added to the ftrace event.
  ASSERT_TRUE(redacted_event.has_sched_process_free());
  ASSERT_TRUE(redacted_event.sched_process_free().has_comm());
  ASSERT_TRUE(redacted_event.sched_process_free().comm().empty());
}

}  // namespace perfetto::trace_redaction
