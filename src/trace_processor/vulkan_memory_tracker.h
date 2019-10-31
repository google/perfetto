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

#ifndef SRC_TRACE_PROCESSOR_VULKAN_MEMORY_TRACKER_H_
#define SRC_TRACE_PROCESSOR_VULKAN_MEMORY_TRACKER_H_

#include <deque>

#include "src/trace_processor/importers/proto/proto_incremental_state.h"
#include "src/trace_processor/trace_storage.h"

#include "protos/perfetto/trace/gpu/vulkan_memory_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class VulkanMemoryTracker {
 public:
  using SourceStringId = uint64_t;

  explicit VulkanMemoryTracker(TraceProcessorContext* context);
  ~VulkanMemoryTracker() = default;

  template <int32_t FieldId>
  StringId GetInternedString(PacketSequenceState* state,
                             size_t generation,
                             uint64_t iid) {
    auto* decoder =
        state->LookupInternedMessage<FieldId, protos::pbzero::InternedString>(
            generation, iid);
    if (!decoder)
      return kNullStringId;
    return context_->storage->InternString(
        base::StringView(reinterpret_cast<const char*>(decoder->str().data),
                         decoder->str().size));
  }

  StringId FindSourceString(SourceStringId);
  StringId FindTypeString(SourceStringId);

 private:
  TraceProcessorContext* const context_;

  std::unordered_map<SourceStringId, StringId> source_string_map_;
  std::unordered_map<SourceStringId, StringId> type_string_map_;

  void SetupSourceAndTypeInternedStrings();
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VULKAN_MEMORY_TRACKER_H_
