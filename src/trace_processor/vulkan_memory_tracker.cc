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

#include "src/trace_processor/vulkan_memory_tracker.h"

#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

VulkanMemoryTracker::VulkanMemoryTracker(TraceProcessorContext* context)
    : context_(context), empty_(context_->storage->InternString({"", 0})) {
  SetupSourceAndTypeInternedStrings();
}

void VulkanMemoryTracker::SetupSourceAndTypeInternedStrings() {
  // It seems a good idea to have the source and type of the event coded as
  // a string id inside the Perfetto db instead of having the enum value
  // stored. However, it seems that Perfetto only allows protobufs which are
  // optimized for LITE_RUNTIME (removing the line results in link errors).
  // Apparently, there is also a minimal implementation of protobuf descriptor
  // in the code base, but it does not have the reflection to retrieve the name
  // of the enum. More investigation is required to resolve this.

  // TODO (zakerinasab): Fix and uncomment the following code to avoid
  // hardcoding the interned strings for the source and type of memory events.
  // const protos::pbzero::EnumDescriptor* source_descriptor =
  //     protos::pbzero::VulkanMemoryEvent::Source_descriptor();
  // for (int i = 0; i < source_descriptor->value_count(); i++) {
  //   auto source_enum = source_descriptor->value(i);
  //   auto source_str = source_enum->name();
  //   auto str_id = context_->storage->InternString(
  //       base::StringView(source_str.c_str(), source_str.length()));
  //   source_string_map_.emplace(source_enum->number(), str_id);
  // }

  // const protos::pbzero::EnumDescriptor* type_descriptor =
  //     protos::pbzero::VulkanMemoryEvent::Type_descriptor();
  // for (int i = 0; i < type_descriptor->value_count(); i++) {
  //   auto type_enum = type_descriptor->value(i);
  //   auto type_str = type_enum->name();
  //   auto str_id = context_->storage->InternString(
  //       base::StringView(type_str.c_str(), type_str.length()));
  //   type_string_map_.emplace(type_enum->number(), str_id);
  // }

  std::unordered_map<int, std::string> event_sources({
      {0, "UNKNOWN_SOURCE"},
      {1, "DEVICE"},
      {2, "HOST"},
      {3, "GPU_DEVICE_MEMORY"},
      {4, "GPU_BUFFER"},
      {5, "GPU_IMAGE"},
  });
  for (const auto& source : event_sources) {
    source_string_map_.emplace(
        source.first, context_->storage->InternString(base::StringView(
                          source.second.c_str(), source.second.length())));
  }

  std::unordered_map<int, std::string> event_types({
      {0, "UNKNOWN_TYPE"},
      {1, "CREATE"},
      {2, "DESTROY"},
      {3, "BIND"},
      {4, "DESTROY_BOUND"},
      {5, "ANNOTATIONS"},
  });
  for (const auto& type : event_types) {
    type_string_map_.emplace(type.first,
                             context_->storage->InternString(base::StringView(
                                 type.second.c_str(), type.second.length())));
  }
}

void VulkanMemoryTracker::AddString(SourceStringId id, StringId str) {
  string_map_.emplace(id, str);
}

base::Optional<StringId> VulkanMemoryTracker::FindString(SourceStringId id) {
  base::Optional<StringId> res;
  if (id == 0) {
    res = empty_;
    return res;
  }
  auto it = string_map_.find(id);
  if (it == string_map_.end()) {
    context_->storage->IncrementStats(
        stats::vulkan_allocations_invalid_string_id);
    PERFETTO_DFATAL("Invalid string.");
    return res;
  }
  res = it->second;
  return res;
}

base::Optional<StringId> VulkanMemoryTracker::FindSourceString(
    SourceStringId source) {
  base::Optional<StringId> res = empty_;
  auto it = source_string_map_.find(source);
  if (it == source_string_map_.end()) {
    context_->storage->IncrementStats(
        stats::vulkan_allocations_invalid_string_id);
    PERFETTO_DFATAL("Invalid  memory event source string.");
    return res;
  }
  res = it->second;
  return res;
}

base::Optional<StringId> VulkanMemoryTracker::FindTypeString(
    SourceStringId type) {
  base::Optional<StringId> res = empty_;
  auto it = type_string_map_.find(type);
  if (it == type_string_map_.end()) {
    context_->storage->IncrementStats(
        stats::vulkan_allocations_invalid_string_id);
    PERFETTO_DFATAL("Invalid  memory event type string.");
    return res;
  }
  res = it->second;
  return res;
}

}  // namespace trace_processor
}  // namespace perfetto
