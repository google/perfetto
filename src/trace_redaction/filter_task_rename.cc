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

#include "src/trace_redaction/filter_task_rename.h"

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

base::Status FilterTaskRename::VerifyContext(const Context& context) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("FilterTaskRename: missing package uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("FilterTaskRename: missing timeline.");
  }

  return base::OkStatus();
}

bool FilterTaskRename::KeepEvent(const Context& context,
                                 protozero::ConstBytes bytes) const {
  PERFETTO_DCHECK(context.package_uid.has_value());
  PERFETTO_DCHECK(context.timeline);

  protozero::ProtoDecoder event_decoder(bytes);

  auto rename = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kTaskRenameFieldNumber);

  // Likely - most events will not have a rename event (that's okay).
  if (!rename.valid()) {
    return true;
  }

  auto pid =
      event_decoder.FindField(protos::pbzero::FtraceEvent::kPidFieldNumber);

  // Unlikely - all events should have a pid.
  if (!pid.valid()) {
    return false;
  }

  auto timestamp = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kTimestampFieldNumber);

  // Unlikely - all events should have a timestamp.
  if (!timestamp.valid()) {
    return false;
  }

  auto slice = context.timeline->Search(timestamp.as_uint64(), pid.as_int32());
  return slice.uid == context.package_uid.value();
}

}  // namespace perfetto::trace_redaction
