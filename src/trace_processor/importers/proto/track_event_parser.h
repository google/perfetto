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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PARSER_H_

#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_storage.h"

#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

class PacketSequenceState;
class TraceProcessorContext;

class TrackEventParser {
 public:
  explicit TrackEventParser(TraceProcessorContext* context);

  void ParseTrackEvent(int64_t ts,
                       int64_t tts,
                       int64_t ticount,
                       PacketSequenceState*,
                       size_t sequence_state_generation,
                       protozero::ConstBytes);
  void ParseLegacyEventAsRawEvent(
      int64_t ts,
      int64_t tts,
      int64_t ticount,
      base::Optional<UniqueTid> utid,
      StringId category_id,
      StringId name_id,
      const protos::pbzero::TrackEvent::LegacyEvent::Decoder& legacy_event,
      SliceTracker::SetArgsCallback args_callback);
  void ParseDebugAnnotationArgs(protozero::ConstBytes debug_annotation,
                                PacketSequenceState*,
                                size_t sequence_state_generation,
                                ArgsTracker* args_tracker,
                                RowId row);
  void ParseNestedValueArgs(protozero::ConstBytes nested_value,
                            base::StringView flat_key,
                            base::StringView key,
                            ArgsTracker* args_tracker,
                            RowId row);
  void ParseTaskExecutionArgs(protozero::ConstBytes task_execution,
                              PacketSequenceState*,
                              size_t sequence_state_generation,
                              ArgsTracker* args_tracker,
                              RowId row);
  void ParseLogMessage(protozero::ConstBytes,
                       PacketSequenceState*,
                       size_t sequence_state_generation,
                       int64_t,
                       base::Optional<UniqueTid>,
                       ArgsTracker*,
                       RowId);

 private:
  TraceProcessorContext* context_;

  const StringId task_file_name_args_key_id_;
  const StringId task_function_name_args_key_id_;
  const StringId task_line_number_args_key_id_;
  const StringId log_message_body_key_id_;
  const StringId raw_legacy_event_id_;
  const StringId legacy_event_category_key_id_;
  const StringId legacy_event_name_key_id_;
  const StringId legacy_event_phase_key_id_;
  const StringId legacy_event_duration_ns_key_id_;
  const StringId legacy_event_thread_timestamp_ns_key_id_;
  const StringId legacy_event_thread_duration_ns_key_id_;
  const StringId legacy_event_thread_instruction_count_key_id_;
  const StringId legacy_event_thread_instruction_delta_key_id_;
  const StringId legacy_event_use_async_tts_key_id_;
  const StringId legacy_event_unscoped_id_key_id_;
  const StringId legacy_event_global_id_key_id_;
  const StringId legacy_event_local_id_key_id_;
  const StringId legacy_event_id_scope_key_id_;
  const StringId legacy_event_bind_id_key_id_;
  const StringId legacy_event_bind_to_enclosing_key_id_;
  const StringId legacy_event_flow_direction_key_id_;
  const StringId flow_direction_value_in_id_;
  const StringId flow_direction_value_out_id_;
  const StringId flow_direction_value_inout_id_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PARSER_H_
