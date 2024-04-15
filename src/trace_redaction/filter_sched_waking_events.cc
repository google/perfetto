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

#include "src/trace_redaction/filter_sched_waking_events.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

base::Status FilterSchedWakingEvents::VerifyContext(
    const Context& context) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("FilterSchedWakingEvents: missing packet uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("FilterSchedWakingEvents: missing timeline.");
  }

  return base::OkStatus();
}

bool FilterSchedWakingEvents::KeepEvent(const Context& context,
                                        protozero::ConstBytes bytes) const {
  PERFETTO_DCHECK(context.package_uid.has_value());
  PERFETTO_DCHECK(context.timeline);

  protozero::ProtoDecoder event_decoder(bytes);

  auto sched_waking = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);

  if (!sched_waking.valid()) {
    return true;  // Keep
  }

  auto timestamp = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  if (!timestamp.valid()) {
    return false;  // Remove
  }

  auto outer_pid =
      event_decoder.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber);

  if (!outer_pid.valid()) {
    return false;  // Remove
  }

  auto outer_slice = context.timeline->Search(
      timestamp.as_uint64(), static_cast<int32_t>(outer_pid.as_uint32()));

  if (outer_slice.uid != context.package_uid.value()) {
    return false;  // Remove
  }

  protozero::ProtoDecoder waking_decoder(sched_waking.as_bytes());

  auto inner_pid = waking_decoder.FindField(
      protos::pbzero::SchedWakingFtraceEvent::kPidFieldNumber);

  if (!inner_pid.valid()) {
    return false;  // Remove
  }

  auto inner_slice =
      context.timeline->Search(timestamp.as_uint64(), inner_pid.as_int32());
  return inner_slice.uid == context.package_uid.value();
}

}  // namespace perfetto::trace_redaction
