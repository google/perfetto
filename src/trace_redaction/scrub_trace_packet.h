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

#include "include/perfetto/base/flat_set.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Drops whole trace packets based on an allow-list (e.g. retain ProcessTree
// packets).
class ScrubTracePacket final : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

 private:
  // TODO(vaage): Most the allow-list into the context and populate it with a
  // build primitive. This will allow for a configurable list.
  base::FlatSet<uint32_t> allow_list_ = {
      protos::pbzero::TracePacket::kProcessTreeFieldNumber,
      protos::pbzero::TracePacket::kProcessStatsFieldNumber,
      protos::pbzero::TracePacket::kClockSnapshotFieldNumber,
      protos::pbzero::TracePacket::kSysStatsFieldNumber,
      protos::pbzero::TracePacket::kTraceConfigFieldNumber,
      protos::pbzero::TracePacket::kTraceStatsFieldNumber,
      protos::pbzero::TracePacket::kSystemInfoFieldNumber,
      protos::pbzero::TracePacket::kTriggerFieldNumber,
      protos::pbzero::TracePacket::kCpuInfoFieldNumber,
      protos::pbzero::TracePacket::kServiceEventFieldNumber,
      protos::pbzero::TracePacket::kInitialDisplayStateFieldNumber,
      protos::pbzero::TracePacket::kFrameTimelineEventFieldNumber,
      protos::pbzero::TracePacket::kAndroidSystemPropertyFieldNumber,
      protos::pbzero::TracePacket::kSynchronizationMarkerFieldNumber,
      protos::pbzero::TracePacket::kFtraceEventsFieldNumber,
  };
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_
