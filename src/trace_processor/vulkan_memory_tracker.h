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

#include "protos/perfetto/trace/gpu/vulkan_memory_event.pbzero.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class VulkanMemoryTracker {
 public:
  using SourceStringId = uint64_t;

  explicit VulkanMemoryTracker(TraceProcessorContext* context);
  ~VulkanMemoryTracker() = default;

  void AddString(SourceStringId, StringId);

  base::Optional<StringId> FindString(SourceStringId);
  base::Optional<StringId> FindSourceString(SourceStringId source);
  base::Optional<StringId> FindTypeString(SourceStringId type);

 private:
  TraceProcessorContext* const context_;
  const StringId empty_;

  std::unordered_map<SourceStringId, StringId> string_map_;
  std::unordered_map<SourceStringId, StringId> source_string_map_;
  std::unordered_map<SourceStringId, StringId> type_string_map_;

  void SetupSourceAndTypeInternedStrings();
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VULKAN_MEMORY_TRACKER_H_
