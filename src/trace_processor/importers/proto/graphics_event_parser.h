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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_PARSER_H_

#include <vector>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/proto_incremental_state.h"
#include "src/trace_processor/trace_storage.h"
#include "src/trace_processor/vulkan_memory_tracker.h"

#include "protos/perfetto/trace/gpu/vulkan_memory_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

struct ProtoEnumHasher {
  template <typename T>
  std::size_t operator()(T t) const {
    return static_cast<std::size_t>(t);
  }
};

// Class for parsing graphics related events.
class GraphicsEventParser {
 public:
  using ConstBytes = protozero::ConstBytes;
  using VulkanMemoryEventSource = VulkanMemoryEvent::Source;
  using VulkanMemoryEventOperation = VulkanMemoryEvent::Operation;
  explicit GraphicsEventParser(TraceProcessorContext*);

  void ParseGpuCounterEvent(int64_t ts, ConstBytes);
  void ParseGpuRenderStageEvent(int64_t ts, ConstBytes);
  void ParseGraphicsFrameEvent(int64_t timestamp, ConstBytes);
  void ParseGpuLog(int64_t ts, ConstBytes);

  void ParseVulkanMemoryEvent(PacketSequenceState*,
                              size_t sequence_state_generation,
                              ConstBytes);
  void UpdateVulkanMemoryAllocationCounters(UniquePid,
                                            const VulkanMemoryEvent::Decoder&);

 private:
  TraceProcessorContext* const context_;
  // For GpuCounterEvent
  std::unordered_map<uint32_t, TrackId> gpu_counter_track_ids_;
  // For GpuRenderStageEvent
  const StringId gpu_render_stage_scope_id_;
  std::vector<TrackId> gpu_hw_queue_ids_;
  std::vector<StringId> gpu_render_stage_ids_;
  // For GraphicsFrameEvent
  const StringId graphics_event_scope_id_;
  const StringId unknown_event_name_id_;
  const StringId no_layer_name_name_id_;
  const StringId layer_name_key_id_;
  std::array<StringId, 14> event_type_name_ids_;
  // For VulkanMemoryEvent
  std::unordered_map<VulkanMemoryEvent::AllocationScope,
                     int64_t /*counter_value*/,
                     ProtoEnumHasher>
      vulkan_driver_memory_counters_;
  std::unordered_map<uint32_t /*memory_type*/, int64_t /*counter_value*/>
      vulkan_device_memory_counters_allocate_;
  std::unordered_map<uint32_t /*memory_type*/, int64_t /*counter_value*/>
      vulkan_device_memory_counters_bind_;
  // For GpuLog
  const StringId gpu_log_track_name_id_;
  const StringId gpu_log_scope_id_;
  const StringId tag_id_;
  const StringId log_message_id_;
  std::array<StringId, 7> log_severity_ids_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_PARSER_H_
