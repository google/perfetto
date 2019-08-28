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

#ifndef SRC_TRACE_PROCESSOR_GRAPHICS_FRAME_EVENT_PARSER_H_
#define SRC_TRACE_PROCESSOR_GRAPHICS_FRAME_EVENT_PARSER_H_

#include <vector>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Class for parsing GraphicFrameEvents.
class GraphicsFrameEventParser {
 public:
  explicit GraphicsFrameEventParser(TraceProcessorContext*);
  ~GraphicsFrameEventParser();

  void ParseEvent(int64_t timestamp, protozero::ConstBytes);

 private:
  TraceProcessorContext* const context_;
  const StringId graphics_event_scope_id_;
  const StringId unspecified_event_name_id_;
  const StringId dequeue_name_id_;
  const StringId queue_name_id_;
  const StringId post_name_id_;
  const StringId acquire_name_id_;
  const StringId latch_name_id_;
  const StringId hwc_composition_queued_name_id_;
  const StringId fallback_composition_name_id_;
  const StringId present_name_id_;
  const StringId release_name_id_;
  const StringId modify_name_id_;
  const StringId unknown_event_name_id_;
  const StringId no_layer_name_name_id_;
  const StringId layer_name_key_id_;
  const StringId frame_number_key_id_;

  std::array<StringId, 11> event_type_name_ids_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_GRAPHICS_FRAME_EVENT_PARSER_H_
