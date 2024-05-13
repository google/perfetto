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

#include "src/trace_redaction/redact_process_free.h"

#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

// Redact sched_process_free events.
//
//  event {
//    timestamp: 6702094703928940
//    pid: 10
//    sched_process_free {
//      comm: "sh"
//      pid: 7973
//      prio: 120
//    }
//  }
//
// In the above message, it should be noted that "event.pid" will not be
// equal to "event.sched_process_free.pid".
//
// The timeline treats "start" as inclusive and "end" as exclusive. This means
// no pid will connect to the target package at a process free event. Because
// of this, the timeline is not needed.
base::Status RedactProcessFree::Redact(
    const Context&,
    const protos::pbzero::FtraceEventBundle::Decoder&,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  auto sched_process_free = event.FindField(
      protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber);
  if (!sched_process_free.valid()) {
    return base::ErrStatus(
        "RedactProcessFree: was used for unsupported field type");
  }

  // SchedProcessFreeFtraceEvent
  protozero::ProtoDecoder process_free_decoder(sched_process_free.as_bytes());

  auto* process_free_message = event_message->set_sched_process_free();

  // Replace the comm with an empty string instead of dropping the comm field.
  // The perfetto UI doesn't render things correctly if comm values are missing.
  for (auto field = process_free_decoder.ReadField(); field.valid();
       field = process_free_decoder.ReadField()) {
    if (field.id() ==
        protos::pbzero::SchedProcessFreeFtraceEvent::kCommFieldNumber) {
      process_free_message->set_comm("");
    } else {
      proto_util::AppendField(field, process_free_message);
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
