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
RedactProcessFree::RedactProcessFree()
    : FtraceEventRedaction(
          protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber) {}

base::Status RedactProcessFree::Redact(
    const Context&,
    const protos::pbzero::FtraceEvent::Decoder&,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEvent* event_message) const {
  protos::pbzero::SchedProcessFreeFtraceEvent::Decoder process_free(bytes);

  // There must be pid. If there's no pid, dropping the event is the safest
  // option.
  if (!process_free.has_pid()) {
    return base::OkStatus();
  }

  // Avoid making the message until we know that we have prev and next pids.
  auto* process_free_message = event_message->set_sched_process_free();

  // To read the fields, move the read head back to the start.
  process_free.Reset();

  for (auto field = process_free.ReadField(); field.valid();
       field = process_free.ReadField()) {
    if (field.id() !=
        protos::pbzero::SchedProcessFreeFtraceEvent::kCommFieldNumber) {
      proto_util::AppendField(field, process_free_message);
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
