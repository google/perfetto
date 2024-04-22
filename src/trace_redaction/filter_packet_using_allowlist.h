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

#ifndef SRC_TRACE_REDACTION_FILTER_PACKET_USING_ALLOWLIST_H_
#define SRC_TRACE_REDACTION_FILTER_PACKET_USING_ALLOWLIST_H_

#include "src/trace_redaction/scrub_trace_packet.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Since the number of allow-listed message types, and the allow-list is
// small, the look-up can be considered constant time.
//
// There is a constant max number of fields in a packet. Given this limit and
// the constant allow-list look-up, this primitive can be considered linear.
class FilterPacketUsingAllowlist : public TracePacketFilter {
 public:
  base::Status VerifyContext(const Context& context) const override;

  bool KeepField(const Context& context,
                 const protozero::Field& field) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_FILTER_PACKET_USING_ALLOWLIST_H_
