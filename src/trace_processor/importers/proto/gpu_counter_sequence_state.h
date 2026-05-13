/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GPU_COUNTER_SEQUENCE_STATE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GPU_COUNTER_SEQUENCE_STATE_H_

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Per-incremental-state cache of which interned counter descriptors have
// already had their custom counter groups inserted. Lives on
// `IncrementalState` so it shares scope with the interned-data table that
// the descriptors themselves are looked up from — keying by iid alone is
// safe here because two distinct producers necessarily live on distinct
// packet sequences (and therefore distinct IncrementalStates), so they each
// get their own cache.
struct GpuCounterSequenceState : PacketSequenceStateGeneration::CustomState {
  explicit GpuCounterSequenceState(TraceProcessorContext*);
  ~GpuCounterSequenceState() override;

  base::FlatHashMap<uint64_t, bool> custom_groups_inserted;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GPU_COUNTER_SEQUENCE_STATE_H_
