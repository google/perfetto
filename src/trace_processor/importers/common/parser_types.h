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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_PARSER_TYPES_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_PARSER_TYPES_H_

#include <stdint.h>

#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"

namespace perfetto {
namespace trace_processor {

struct alignas(8) InlineSchedSwitch {
  int64_t prev_state;
  int32_t next_pid;
  int32_t next_prio;
  StringPool::Id next_comm;
};

struct alignas(8) InlineSchedWaking {
  int32_t pid;
  int32_t target_cpu;
  int32_t prio;
  StringPool::Id comm;
};

struct alignas(8) JsonEvent {
  std::string value;
};

struct TracePacketData {
  TraceBlobView packet;
  RefPtr<PacketSequenceStateGeneration> sequence_state;
};

struct TrackEventData {
  TrackEventData(TraceBlobView pv,
                 RefPtr<PacketSequenceStateGeneration> generation)
      : trace_packet_data{std::move(pv), std::move(generation)} {}

  explicit TrackEventData(TracePacketData tpd)
      : trace_packet_data(std::move(tpd)) {}

  static constexpr uint8_t kMaxNumExtraCounters = 8;

  uint8_t CountExtraCounterValues() const {
    for (uint8_t i = 0; i < TrackEventData::kMaxNumExtraCounters; ++i) {
      if (std::equal_to<double>()(extra_counter_values[i], 0))
        return i;
    }
    return TrackEventData::kMaxNumExtraCounters;
  }

  TracePacketData trace_packet_data;
  base::Optional<int64_t> thread_timestamp;
  base::Optional<int64_t> thread_instruction_count;
  double counter_value = 0;
  std::array<double, kMaxNumExtraCounters> extra_counter_values = {};
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_PARSER_TYPES_H_
