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

#include "src/trace_redaction/filter_print_events.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

base::Status FilterPrintEvents::VerifyContext(const Context& context) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("FilterPrintEvents: missing packet uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("FilterPrintEvents: missing timeline.");
  }

  return base::OkStatus();
}

bool FilterPrintEvents::KeepEvent(const Context& context,
                                  protozero::ConstBytes bytes) const {
  PERFETTO_DCHECK(context.timeline);
  PERFETTO_DCHECK(context.package_uid.has_value());

  const auto* timeline = context.timeline.get();
  auto package_uid = context.package_uid;

  protozero::ProtoDecoder event(bytes);

  // This is not a print packet. Keep the packet.
  if (!event.FindField(protos::pbzero::FtraceEvent::kPrintFieldNumber)
           .valid()) {
    return true;
  }

  auto time =
      event.FindField(protos::pbzero::FtraceEvent::kTimestampFieldNumber);
  auto pid = event.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber);

  // Pid + Time --> UID, if the uid matches the target package, keep the event.
  return pid.valid() && time.valid() &&
         timeline->Search(time.as_uint64(), pid.as_int32()).uid == package_uid;
}

}  // namespace perfetto::trace_redaction
