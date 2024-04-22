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

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

base::Status FilterPacketUsingAllowlist::VerifyContext(
    const Context& context) const {
  if (context.trace_packet_allow_list.empty()) {
    return base::ErrStatus("FilterPacketUsingAllowlist: missing allow-list.");
  }

  return base::OkStatus();
}

bool FilterPacketUsingAllowlist::KeepField(
    const Context& context,
    const protozero::Field& field) const {
  PERFETTO_DCHECK(!context.trace_packet_allow_list.empty());
  return field.valid() && context.trace_packet_allow_list.count(field.id());
}

}  // namespace perfetto::trace_redaction
