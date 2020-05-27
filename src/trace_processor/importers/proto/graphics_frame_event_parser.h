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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_FRAME_EVENT_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_FRAME_EVENT_PARSER_H_

#include <vector>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/proto_incremental_state.h"
#include "src/trace_processor/importers/proto/vulkan_memory_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/android/graphics_frame_event.pbzero.h"

namespace perfetto {

namespace trace_processor {

class TraceProcessorContext;

// Class for parsing graphics frame related events.
class GraphicsFrameEventParser {
 public:
  using ConstBytes = protozero::ConstBytes;
  explicit GraphicsFrameEventParser(TraceProcessorContext*);

  void ParseGraphicsFrameEvent(int64_t timestamp, ConstBytes);

 private:
  using GraphicsFrameEventDecoder =
      protos::pbzero::GraphicsFrameEvent_BufferEvent_Decoder;
  using GraphicsFrameEvent = protos::pbzero::GraphicsFrameEvent;
  bool CreateBufferEvent(int64_t timestamp, GraphicsFrameEventDecoder& event);
  void CreatePhaseEvent(int64_t timestamp, GraphicsFrameEventDecoder& event);

  TraceProcessorContext* const context_;
  const StringId graphics_event_scope_id_;
  const StringId unknown_event_name_id_;
  const StringId no_layer_name_name_id_;
  const StringId layer_name_key_id_;
  std::array<StringId, 14> event_type_name_ids_;
  const StringId queue_lost_message_id_;
  // Map of buffer ID -> slice id of the dequeue event
  std::unordered_map<uint32_t, SliceId> dequeue_slice_ids_;

  // Row indices of frame stats table. Used to populate the slice_id after
  // inserting the rows.
  std::vector<uint32_t> graphics_frame_stats_idx_;
  // Map of buffer ID -> (Map of GraphicsFrameEvent -> ts of that event)
  std::unordered_map<uint32_t, std::unordered_map<uint64_t, int64_t>>
      graphics_frame_stats_map_;

  // Maps of buffer id -> track id
  std::unordered_map<uint32_t, TrackId> dequeue_map_;
  std::unordered_map<uint32_t, TrackId> queue_map_;
  std::unordered_map<uint32_t, TrackId> latch_map_;
  // Map of layer name -> track id
  std::unordered_map<StringId, TrackId> display_map_;

  // Maps of buffer id -> timestamp
  std::unordered_map<uint32_t, int64_t> last_dequeued_;
  std::unordered_map<uint32_t, int64_t> last_acquired_;
};
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_FRAME_EVENT_PARSER_H_
