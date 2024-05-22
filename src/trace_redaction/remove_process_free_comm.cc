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

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

base::Status RemoveProcessFreeComm::Redact(
    const Context&,
    const protos::pbzero::FtraceEventBundle::Decoder&,
    protozero::ProtoDecoder& event,
    protos::pbzero::FtraceEvent* event_message) const {
  auto sched_process_free = event.FindField(
      protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber);
  if (!sched_process_free.valid()) {
    return base::ErrStatus("RemoveProcessFreeComm: missing required field.");
  }

  // SchedProcessFreeFtraceEvent
  protozero::ProtoDecoder decoder(sched_process_free.as_bytes());

  auto pid_field = decoder.FindField(
    protos::pbzero::SchedProcessFreeFtraceEvent::kPidFieldNumber);
  auto prio_field = decoder.FindField(
    protos::pbzero::SchedProcessFreeFtraceEvent::kPrioFieldNumber);

  if (!pid_field.valid() || !prio_field.valid()) {
    return base::ErrStatus("RemoveProcessFreeComm: missing required field.");
  }

  auto* message = event_message->set_sched_process_free();

  // Replace the comm with an empty string instead of dropping the comm field.
  // The perfetto UI doesn't render things correctly if comm values are missing.
  message->set_comm("");
  message->set_pid(pid_field.as_int32());
  message->set_prio(prio_field.as_int32());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
