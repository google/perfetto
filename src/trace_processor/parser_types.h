/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PARSER_TYPES_H_
#define SRC_TRACE_PROCESSOR_PARSER_TYPES_H_

#include <stdint.h>

#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

struct InlineSchedSwitch {
  int64_t prev_state;
  int32_t next_pid;
  int32_t next_prio;
  StringId next_comm;
};

struct InlineSchedWaking {
  int32_t pid;
  int32_t target_cpu;
  int32_t prio;
  StringId comm;
};

struct TracePacketData {
  TraceBlobView packet;
  RefPtr<PacketSequenceStateGeneration> sequence_state;
};

struct FtraceEventData {
  TraceBlobView event;
  RefPtr<PacketSequenceStateGeneration> sequence_state;
};

struct TrackEventData : public TracePacketData {
  TrackEventData(TraceBlobView pv,
                 RefPtr<PacketSequenceStateGeneration> generation)
      : TracePacketData{std::move(pv), std::move(generation)} {}

  explicit TrackEventData(TracePacketData tpd)
      : TracePacketData(std::move(tpd)) {}

  static constexpr size_t kMaxNumExtraCounters = 8;

  base::Optional<int64_t> thread_timestamp;
  base::Optional<int64_t> thread_instruction_count;
  double counter_value = 0;
  std::array<double, kMaxNumExtraCounters> extra_counter_values = {};
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PARSER_TYPES_H_
