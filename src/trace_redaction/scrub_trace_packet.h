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

#ifndef SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_
#define SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// TODO(vaage): This primitive DOES NOT handle redacting sched_switch and
// sched_waking ftrace events. These events contain a large amount of "to be
// redacted" information AND there are a high quantity of them AND they are
// large packets. As such, this primitive is not enough and an ADDITIONAL
// primitive is required.

// Goes through individual ftrace packs and drops the ftrace packets from the
// trace packet without modifying the surround fields.
//
// ScrubTracePacket does not respect field order - i.e. the field order going
// may not match the field order going out.
class ScrubTracePacket final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_
