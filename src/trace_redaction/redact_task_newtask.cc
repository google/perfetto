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

#include <string>

#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"

namespace perfetto::trace_redaction {

// Redact sched switch trace events in an ftrace event bundle:
//
// event {
//   timestamp: 6702094133317685
//   pid: 6167
//   task_newtask {
//     pid: 7972
//     comm: "adbd"
//     clone_flags: 4001536
//     oom_score_adj: -1000
//   }
// }
//
// In the above message, it should be noted that "event.pid" will never be
// equal to "event.task_newtask.pid" (a thread cannot start itself).

// TODO(vaage): How does this primitive (and others like it) work when we're
// merging threads? Remame events are already dropped. New task and proces free
// events won't matter the timeline is created. Can these events be dropped?

RedactTaskNewTask::RedactTaskNewTask()
    : FtraceEventRedaction(
          protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber) {}

base::Status RedactTaskNewTask::Redact(
    const Context& context,
    const protos::pbzero::FtraceEvent::Decoder& event,
    protozero::ConstBytes bytes,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactTaskNewTask: missing package uid");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactTaskNewTask: missing timeline");
  }

  // There must be a pid. If not, the message is meaningless and can be dropped.
  if (!event.has_timestamp()) {
    return base::OkStatus();
  }

  protozero::ProtoDecoder new_task(bytes);

  auto pid = new_task.FindField(
      protos::pbzero::TaskNewtaskFtraceEvent::kPidFieldNumber);

  if (!pid.valid()) {
    return base::OkStatus();
  }

  // Avoid making the message until we know that we have prev and next pids.
  auto* new_task_message = event_message->set_task_newtask();

  auto slice = context.timeline->Search(event.timestamp(), pid.as_int32());

  for (auto field = new_task.ReadField(); field.valid();
       field = new_task.ReadField()) {
    if (field.id() ==
        protos::pbzero::TaskNewtaskFtraceEvent::kCommFieldNumber) {
      if (slice.uid == context.package_uid) {
        proto_util::AppendField(field, new_task_message);
      } else {
        // Perfetto view (ui.perfetto.dev) crashes if the comm value is missing.
        // To work around this, the comm value is replaced with an empty string.
        // This appears to work.
        new_task_message->set_comm("");
      }
    } else {
      proto_util::AppendField(field, new_task_message);
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
