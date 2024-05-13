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

#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

protozero::ConstChars SanitizeCommValue(const Context& context,
                                        ProcessThreadTimeline::Slice slice,
                                        protozero::Field field) {
  if (NormalizeUid(slice.uid) == NormalizeUid(context.package_uid.value())) {
    return field.as_string();
  }

  return {};
}

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
    const protos::pbzero::FtraceEventBundle::Decoder&,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("RedactTaskNewTask: missing package uid");
  }

  if (!context.timeline) {
    return base::ErrStatus("RedactTaskNewTask: missing timeline");
  }

  // The timestamp is needed to do the timeline look-up. If the packet has no
  // timestamp, don't add the sched switch event. This is the safest option.
  auto timestamp =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);
  if (!timestamp.valid()) {
    return base::OkStatus();
  }

  auto new_task =
      event.FindField(protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber);
  if (!new_task.valid()) {
    return base::ErrStatus(
        "RedactTaskNewTask: was used for unsupported field type");
  }

  protozero::ProtoDecoder new_task_decoder(new_task.as_bytes());

  auto pid = new_task_decoder.FindField(
      protos::pbzero::TaskNewtaskFtraceEvent::kPidFieldNumber);

  if (!pid.valid()) {
    return base::OkStatus();
  }

  // Avoid making the message until we know that we have prev and next pids.
  auto* new_task_message = event_message->set_task_newtask();

  auto slice = context.timeline->Search(timestamp.as_uint64(), pid.as_int32());

  for (auto field = new_task_decoder.ReadField(); field.valid();
       field = new_task_decoder.ReadField()) {
    // Perfetto view (ui.perfetto.dev) crashes if the comm value is missing.
    // To work around this, the comm value is replaced with an empty string.
    // This appears to work.
    if (field.id() ==
        protos::pbzero::TaskNewtaskFtraceEvent::kCommFieldNumber) {
      new_task_message->set_comm(SanitizeCommValue(context, slice, field));
    } else {
      proto_util::AppendField(field, new_task_message);
    }
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
