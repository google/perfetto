/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_COLLECT_CLOCKS_H_
#define SRC_TRACE_REDACTION_COLLECT_CLOCKS_H_

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class CollectClocks : public CollectPrimitive {
 public:
  base::Status Collect(
      const protos::pbzero::TracePacket::Decoder& packet_decoder,
      Context* context) const override;

 private:
  base::Status OnTracePacketDefaults(
      protozero::ConstBytes trace_packet_defaults_bytes,
      uint32_t trusted_sequence_id,
      Context* context) const;

  base::StatusOr<ClockTimestamp> ParseClock(
      protozero::ConstBytes clock_bytes) const;

  mutable std::vector<RedactorClockSynchronizer::ClockTimestamp>
      clock_snapshot_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_COLLECT_CLOCKS_H_
