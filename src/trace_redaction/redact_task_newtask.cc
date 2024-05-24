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

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_redaction {

namespace {

}  // namespace
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
base::Status RedactTaskNewTask::Redact(
    const Context& context,
    const protos::pbzero::FtraceEventBundle::Decoder& bundle,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  PERFETTO_DCHECK(modifier_);

  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactTaskNewTask: missing package uid");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactTaskNewTask: missing timeline");
  }

  // The timestamp is needed to do the timeline look-up. If the packet has no
  // timestamp, don't add the sched switch event.
  auto timestamp_field =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);
  auto new_task_field =
      event.FindField(protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber);

  if (!timestamp_field.valid() || !new_task_field.valid()) {
    return base::ErrStatus(
        "RedactTaskNewTask: missing required FtraceEvent field.");
  }

  protos::pbzero::TaskNewtaskFtraceEvent::Decoder new_task(
      new_task_field.as_bytes());

  // There are only four fields in a new task event. Since two of them can
  // change, it is easier to work with them directly.
  if (!new_task.has_pid() || !new_task.has_comm() ||
      !new_task.has_clone_flags() || !new_task.has_oom_score_adj()) {
    return base::ErrStatus(
        "RedactTaskNewTask: missing required TaskNewtaskFtraceEvent field.");
  }

  auto pid = new_task.pid();
  auto comm = new_task.comm().ToStdString();

  auto cpu = static_cast<int32_t>(bundle.cpu());

  RETURN_IF_ERROR(modifier_->Modify(context, timestamp_field.as_uint64(), cpu,
                                    &pid, &comm));

  auto* new_task_message = event_message->set_task_newtask();
  new_task_message->set_pid(pid);
  new_task_message->set_comm(comm);
  new_task_message->set_clone_flags(new_task.clone_flags());
  new_task_message->set_oom_score_adj(new_task.oom_score_adj());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
