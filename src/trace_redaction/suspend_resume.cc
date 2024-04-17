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

#include "src/trace_redaction/suspend_resume.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"

namespace perfetto::trace_redaction {

base::Status AllowSuspendResume::Build(Context* context) const {
  context->ftrace_packet_allow_list.insert(
      protos::pbzero::FtraceEvent::kSuspendResumeFieldNumber);

  // Values are taken from "suspend_period.textproto".
  context->suspend_result_allow_list.insert("syscore_suspend");
  context->suspend_result_allow_list.insert("syscore_resume");
  context->suspend_result_allow_list.insert("timekeeping_freeze");

  return base::OkStatus();
}

base::Status FilterSuspendResume::VerifyContext(const Context&) const {
  // FilterSuspendResume could check if kSuspendResumeFieldNumber is present in
  // ftrace_packet_allow_list and there are values in the
  // suspend_result_allow_list, but would make it hard to enable/disable
  // suspend-resume redaction.
  return base::OkStatus();
}

// The ftrace event is passed in.
bool FilterSuspendResume::KeepEvent(const Context& context,
                                    protozero::ConstBytes bytes) const {
  protozero::ProtoDecoder event_decoder(bytes);

  auto suspend_resume = event_decoder.FindField(
      protos::pbzero::FtraceEvent::kSuspendResumeFieldNumber);

  // It's not a suspend-resume event, defer the decision to another filter.
  if (!suspend_resume.valid()) {
    return true;
  }

  protozero::ProtoDecoder suspend_resume_decoder(suspend_resume.as_bytes());

  auto action = suspend_resume_decoder.FindField(
      protos::pbzero::SuspendResumeFtraceEvent::kActionFieldNumber);

  return !action.valid() ||
         context.suspend_result_allow_list.count(action.as_std_string());
}

}  // namespace perfetto::trace_redaction
