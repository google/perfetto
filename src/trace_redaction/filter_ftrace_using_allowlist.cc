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

#include "src/trace_redaction/filter_ftrace_using_allowlist.h"

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"

namespace perfetto::trace_redaction {

base::Status FilterFtraceUsingAllowlist::VerifyContext(
    const Context& context) const {
  if (context.ftrace_packet_allow_list.empty()) {
    return base::ErrStatus(
        "FilterFtraceUsingAllowlist: missing ftrace allowlist.");
  }

  return base::OkStatus();
}

bool FilterFtraceUsingAllowlist::KeepEvent(const Context& context,
                                           protozero::ConstBytes bytes) const {
  PERFETTO_DCHECK(!context.ftrace_packet_allow_list.empty());

  protozero::ProtoDecoder event(bytes);

  for (auto field = event.ReadField(); field.valid();
       field = event.ReadField()) {
    if (context.ftrace_packet_allow_list.count(field.id()) != 0) {
      return true;
    }
  }

  return false;
}

}  // namespace perfetto::trace_redaction
