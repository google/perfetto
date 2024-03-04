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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_SEQUENCE_STATE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_SEQUENCE_STATE_H_

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class StackProfileSequenceState final
    : public PacketSequenceStateGeneration::InternedDataTracker {
 public:
  explicit StackProfileSequenceState(TraceProcessorContext* context);

  StackProfileSequenceState(const StackProfileSequenceState&);

  virtual ~StackProfileSequenceState() override;

  std::optional<MappingId> FindOrInsertMapping(uint64_t iid);
  std::optional<CallsiteId> FindOrInsertCallstack(uint64_t iid);

 private:
  std::optional<base::StringView> LookupInternedBuildId(uint64_t iid);
  std::optional<base::StringView> LookupInternedMappingPath(uint64_t iid);
  std::optional<base::StringView> LookupInternedFunctionName(uint64_t iid);
  std::optional<FrameId> FindOrInsertFrame(uint64_t iid);

  TraceProcessorContext* const context_;
  base::FlatHashMap<uint64_t, MappingId> cached_mappings_;
  base::FlatHashMap<uint64_t, CallsiteId> cached_callstacks_;
  base::FlatHashMap<uint64_t, FrameId> cached_frames_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STACK_PROFILE_SEQUENCE_STATE_H_
