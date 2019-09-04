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

#include "src/trace_processor/graphics_frame_event_parser.h"

#include "perfetto/protozero/field.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/virtual_track_tracker.h"

#include "protos/perfetto/trace/android/graphics_frame_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

using BufferEventType = protos::pbzero::GraphicsFrameEvent_BufferEventType;
using ConstBytes = protozero::ConstBytes;

GraphicsFrameEventParser::~GraphicsFrameEventParser() = default;

GraphicsFrameEventParser::GraphicsFrameEventParser(
    TraceProcessorContext* context)
    : context_(context),
      graphics_event_scope_id_(
          context->storage->InternString("graphics_frame_event.scope")),
      unspecified_event_name_id_(
          context->storage->InternString("unspecified_event")),
      dequeue_name_id_(context->storage->InternString("Dequeue")),
      queue_name_id_(context->storage->InternString("Queue")),
      post_name_id_(context->storage->InternString("Post")),
      acquire_name_id_(context->storage->InternString("AcquireFenceSignaled")),
      latch_name_id_(context->storage->InternString("Latch")),
      hwc_composition_queued_name_id_(
          context->storage->InternString("HWCCompositionQueued")),
      fallback_composition_name_id_(
          context->storage->InternString("FallbackComposition")),
      present_name_id_(context->storage->InternString("PresentFenceSignaled")),
      release_name_id_(context->storage->InternString("ReleaseFenceSignaled")),
      modify_name_id_(context->storage->InternString("Modify")),
      unknown_event_name_id_(context->storage->InternString("unknown_event")),
      no_layer_name_name_id_(context->storage->InternString("no_layer_name")),
      layer_name_key_id_(context->storage->InternString("layer_name")),
      frame_number_key_id_(context->storage->InternString("frame_number")),
      event_type_name_ids_{
          {unspecified_event_name_id_ /* UNSPECIFIED */,
           dequeue_name_id_ /* DEQUEUE */, queue_name_id_ /* QUEUE */,
           post_name_id_ /* POST */, acquire_name_id_ /* ACQUIRE_FENCE */,
           latch_name_id_ /* LATCH */,
           hwc_composition_queued_name_id_ /* HWC_COMPOSITION_QUEUED */,
           fallback_composition_name_id_ /* FALLBACK_COMPOSITION */,
           present_name_id_ /* PRESENT_FENCE */,
           release_name_id_ /* RELEASE_FENCE */,
           modify_name_id_ /* MODIFY */}} {}

void GraphicsFrameEventParser::ParseEvent(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::GraphicsFrameEvent_Decoder frame_event(blob.data, blob.size);
  if (!frame_event.has_buffer_event()) {
    return;
  }

  ConstBytes bufferBlob = frame_event.buffer_event();
  protos::pbzero::GraphicsFrameEvent_BufferEvent_Decoder event(bufferBlob.data,
                                                               bufferBlob.size);

  if (!event.has_buffer_id()) {
    context_->storage->IncrementStats(stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing buffer id field.");
    return;
  }

  StringId event_name_id = unknown_event_name_id_;
  if (event.has_type()) {
    const auto type = static_cast<size_t>(event.type());
    if (type < event_type_name_ids_.size()) {
      event_name_id = event_type_name_ids_[type];
    } else {
      context_->storage->IncrementStats(stats::graphics_frame_event_parser_errors);
      PERFETTO_ELOG("GraphicsFrameEvent with unknown type %zu.", type);
    }
  } else {
    context_->storage->IncrementStats(stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing type field.");
  }

  const uint32_t buffer_id = event.buffer_id();
  StringId layer_name_id;

  char buffer[4096];
  const size_t layerNameMaxLength = 4000;
  base::StringWriter track_name(buffer, sizeof(buffer));
  if (event.has_layer_name()) {
    const base::StringView layer_name(event.layer_name());
    layer_name_id = context_->storage->InternString(layer_name);
    track_name.AppendString(layer_name.substr(0, layerNameMaxLength));
  } else {
    layer_name_id = no_layer_name_name_id_;
    track_name.AppendLiteral("unknown_layer");
  }
  track_name.AppendLiteral("[buffer:");
  track_name.AppendUnsignedInt(buffer_id);
  track_name.AppendChar(']');

  const StringId track_name_id =
      context_->storage->InternString(track_name.GetStringView());
  const int64_t duration =
      event.has_duration_ns() ? static_cast<int64_t>(event.duration_ns()) : 0;
  const uint32_t frame_number =
      event.has_frame_number() ? event.frame_number() : 0;

  const TrackId track_id = context_->virtual_track_tracker->GetOrCreateTrack(
      {VirtualTrackScope::kGlobal, 0 /* upid */, track_name_id.id,
       graphics_event_scope_id_},
      track_name_id);

  context_->storage->mutable_gpu_track_table()->Insert(
      track_id, graphics_event_scope_id_, base::nullopt /* context */);

  const auto slice_id = context_->slice_tracker->Scoped(
      timestamp, track_id, RefType::kRefTrack, 0 /* cat */, event_name_id,
      duration, [this, layer_name_id](ArgsTracker* args_tracker, RowId row_id) {
        args_tracker->AddArg(row_id, layer_name_key_id_, layer_name_key_id_,
                             Variadic::String(layer_name_id));
      });

  if (slice_id) {
    context_->storage->mutable_gpu_slice_table()->Insert(
        slice_id.value(), base::nullopt /* context_id */,
        base::nullopt /* render_target */, frame_number,
        base::nullopt /* job_id */, base::nullopt /* hw_queue_id */);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
