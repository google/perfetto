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

#include "src/trace_processor/importers/proto/graphics_frame_event_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace/android/graphics_frame_event.pbzero.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

constexpr char kQueueLostMessage[] =
    "Missing queue event. The slice is now a bit extended than it might "
    "actually have been";
GraphicsFrameEventParser::GraphicsFrameEventParser(
    TraceProcessorContext* context)
    : context_(context),
      graphics_event_scope_id_(
          context->storage->InternString("graphics_frame_event")),
      unknown_event_name_id_(context->storage->InternString("unknown_event")),
      no_layer_name_name_id_(context->storage->InternString("no_layer_name")),
      layer_name_key_id_(context->storage->InternString("layer_name")),
      event_type_name_ids_{
          {context->storage->InternString(
               "unspecified_event") /* UNSPECIFIED */,
           context->storage->InternString("Dequeue") /* DEQUEUE */,
           context->storage->InternString("Queue") /* QUEUE */,
           context->storage->InternString("Post") /* POST */,
           context->storage->InternString(
               "AcquireFenceSignaled") /* ACQUIRE_FENCE */,
           context->storage->InternString("Latch") /* LATCH */,
           context->storage->InternString(
               "HWCCompositionQueued") /* HWC_COMPOSITION_QUEUED */,
           context->storage->InternString(
               "FallbackComposition") /* FALLBACK_COMPOSITION */,
           context->storage->InternString(
               "PresentFenceSignaled") /* PRESENT_FENCE */,
           context->storage->InternString(
               "ReleaseFenceSignaled") /* RELEASE_FENCE */,
           context->storage->InternString("Modify") /* MODIFY */,
           context->storage->InternString("Detach") /* DETACH */,
           context->storage->InternString("Attach") /* ATTACH */,
           context->storage->InternString("Cancel") /* CANCEL */}},
      queue_lost_message_id_(
          context->storage->InternString(kQueueLostMessage)) {}

bool GraphicsFrameEventParser::CreateBufferEvent(
    int64_t timestamp,
    GraphicsFrameEventDecoder& event) {
  if (!event.has_buffer_id()) {
    context_->storage->IncrementStats(
        stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing buffer id field.");
    return false;
  }

  // Use buffer id + layer name as key because sometimes the same buffer can be
  // used by different layers.
  char event_key_buffer[4096];
  base::StringWriter event_key_str(event_key_buffer,
                                   base::ArraySize(event_key_buffer));
  const uint32_t buffer_id = event.buffer_id();
  StringId layer_name_id;
  event_key_str.AppendUnsignedInt(buffer_id);

  if (event.has_layer_name()) {
    layer_name_id = context_->storage->InternString(event.layer_name());
    event_key_str.AppendString(base::StringView(event.layer_name()));
  } else {
    layer_name_id = no_layer_name_name_id_;
  }
  StringId event_key =
      context_->storage->InternString(event_key_str.GetStringView());

  StringId event_name_id = unknown_event_name_id_;
  if (event.has_type()) {
    const auto type = static_cast<size_t>(event.type());
    if (type < event_type_name_ids_.size()) {
      event_name_id = event_type_name_ids_[type];
      graphics_frame_stats_map_[event_key][type] = timestamp;
    } else {
      context_->storage->IncrementStats(
          stats::graphics_frame_event_parser_errors);
      PERFETTO_ELOG("GraphicsFrameEvent with unknown type %zu.", type);
    }
  } else {
    context_->storage->IncrementStats(
        stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing type field.");
  }

  char buffer[4096];
  base::StringWriter track_name(buffer, base::ArraySize(buffer));
  track_name.AppendLiteral("Buffer: ");
  track_name.AppendUnsignedInt(buffer_id);
  track_name.AppendLiteral(" ");
  track_name.AppendString(base::StringView(event.layer_name()));

  const StringId track_name_id =
      context_->storage->InternString(track_name.GetStringView());
  const int64_t duration =
      event.has_duration_ns() ? static_cast<int64_t>(event.duration_ns()) : 0;
  uint32_t frame_number = event.has_frame_number() ? event.frame_number() : 0;

  tables::GpuTrackTable::Row track(track_name_id);
  track.scope = graphics_event_scope_id_;
  TrackId track_id = context_->track_tracker->LegacyInternGpuTrack(track);

  auto* graphics_frame_slice_table =
      context_->storage->mutable_graphics_frame_slice_table();
  {
    tables::GraphicsFrameSliceTable::Row row;
    row.ts = timestamp;
    row.track_id = track_id;
    row.name = event_name_id;
    row.dur = duration;
    row.frame_number = frame_number;
    row.layer_name = layer_name_id;
    if (event.type() == GraphicsFrameEvent::PRESENT_FENCE) {
      auto acquire_ts =
          graphics_frame_stats_map_[event_key]
                                   [GraphicsFrameEvent::ACQUIRE_FENCE];
      auto queue_ts =
          graphics_frame_stats_map_[event_key][GraphicsFrameEvent::QUEUE];
      auto latch_ts =
          graphics_frame_stats_map_[event_key][GraphicsFrameEvent::LATCH];

      row.queue_to_acquire_time =
          std::max(acquire_ts - queue_ts, static_cast<int64_t>(0));
      row.acquire_to_latch_time = latch_ts - acquire_ts;
      row.latch_to_present_time = timestamp - latch_ts;
    }
    std::optional<SliceId> opt_slice_id =
        context_->slice_tracker->ScopedTyped(graphics_frame_slice_table, row);
    if (event.type() == GraphicsFrameEvent::DEQUEUE) {
      if (opt_slice_id) {
        dequeue_slice_ids_[event_key] = *opt_slice_id;
      }
    } else if (event.type() == GraphicsFrameEvent::QUEUE) {
      auto it = dequeue_slice_ids_.find(event_key);
      if (it != dequeue_slice_ids_.end()) {
        auto rr = graphics_frame_slice_table->FindById(it->second);
        rr->set_frame_number(frame_number);
      }
    }
  }
  return true;
}

void GraphicsFrameEventParser::InvalidatePhaseEvent(int64_t timestamp,
                                                    TrackId track_id,
                                                    bool reset_name) {
  const auto opt_slice_id = context_->slice_tracker->End(timestamp, track_id);

  if (opt_slice_id) {
    auto* graphics_frame_slice_table =
        context_->storage->mutable_graphics_frame_slice_table();
    auto rr = *graphics_frame_slice_table->FindById(*opt_slice_id);
    if (reset_name) {
      // Set the name (frame_number) to be 0 since there is no frame number
      // associated, example : dequeue event.
      StringId frame_name_id = context_->storage->InternString("0");
      rr.set_name(frame_name_id);
      rr.set_frame_number(0);
    }

    // Set the duration to -1 so that this slice will be ignored by the
    // UI. Setting any other duration results in wrong data which we want
    // to avoid at all costs.
    rr.set_dur(-1);
  }
}

// Here we convert the buffer events into Phases(slices)
// APP: Dequeue to Queue
// Wait for GPU: Queue to Acquire
// SurfaceFlinger (SF): Latch to Present
// Display: Present to next Present (of the same layer)
void GraphicsFrameEventParser::CreatePhaseEvent(
    int64_t timestamp,
    GraphicsFrameEventDecoder& event) {
  // Use buffer id + layer name as key because sometimes the same buffer can be
  // used by different layers.
  char event_key_buffer[4096];
  base::StringWriter event_key_str(event_key_buffer,
                                   base::ArraySize(event_key_buffer));
  const uint32_t buffer_id = event.buffer_id();
  uint32_t frame_number = event.has_frame_number() ? event.frame_number() : 0;
  event_key_str.AppendUnsignedInt(buffer_id);
  StringId layer_name_id;
  if (event.has_layer_name()) {
    layer_name_id = context_->storage->InternString(event.layer_name());
    event_key_str.AppendString(base::StringView(event.layer_name()));
  } else {
    layer_name_id = no_layer_name_name_id_;
  }
  StringId event_key =
      context_->storage->InternString(event_key_str.GetStringView());

  char track_buffer[4096];
  char slice_buffer[4096];
  // We'll be using the name StringWriter and name_id for writing track names
  // and slice names.
  base::StringWriter track_name(track_buffer, base::ArraySize(track_buffer));
  base::StringWriter slice_name(slice_buffer, base::ArraySize(slice_buffer));
  StringId track_name_id;
  TrackId track_id;
  bool start_slice = true;

  // Close the previous phase before starting the new phase
  switch (event.type()) {
    case GraphicsFrameEvent::DEQUEUE: {
      track_name.reset();
      track_name.AppendLiteral("APP_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name.AppendLiteral(" ");
      track_name.AppendString(base::StringView(event.layer_name()));
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row app_track(track_name_id);
      app_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->LegacyInternGpuTrack(app_track);

      // Error handling
      auto dequeue_time = dequeue_map_.find(event_key);
      if (dequeue_time != dequeue_map_.end()) {
        InvalidatePhaseEvent(timestamp, dequeue_time->second, true);
        dequeue_map_.erase(dequeue_time);
      }
      auto queue_time = queue_map_.find(event_key);
      if (queue_time != queue_map_.end()) {
        InvalidatePhaseEvent(timestamp, queue_time->second);
        queue_map_.erase(queue_time);
      }

      dequeue_map_[event_key] = track_id;
      last_dequeued_[event_key] = timestamp;
      break;
    }

    case GraphicsFrameEvent::QUEUE: {
      auto dequeue_time = dequeue_map_.find(event_key);
      if (dequeue_time != dequeue_map_.end()) {
        const auto opt_slice_id =
            context_->slice_tracker->End(timestamp, dequeue_time->second);
        slice_name.reset();
        slice_name.AppendUnsignedInt(frame_number);
        if (opt_slice_id) {
          auto* graphics_frame_slice_table =
              context_->storage->mutable_graphics_frame_slice_table();
          // Set the name of the slice to be the frame number since dequeue did
          // not have a frame number at that time.
          auto rr = *graphics_frame_slice_table->FindById(*opt_slice_id);
          rr.set_name(
              context_->storage->InternString(slice_name.GetStringView()));
          rr.set_frame_number(frame_number);
          dequeue_map_.erase(dequeue_time);
        }
      }
      // The AcquireFence might be signaled before receiving a QUEUE event
      // sometimes. In that case, we shouldn't start a slice.
      if (last_acquired_[event_key] > last_dequeued_[event_key] &&
          last_acquired_[event_key] < timestamp) {
        start_slice = false;
        break;
      }
      track_name.reset();
      track_name.AppendLiteral("GPU_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name.AppendLiteral(" ");
      track_name.AppendString(base::StringView(event.layer_name()));
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row gpu_track(track_name_id);
      gpu_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->LegacyInternGpuTrack(gpu_track);
      queue_map_[event_key] = track_id;
      break;
    }
    case GraphicsFrameEvent::ACQUIRE_FENCE: {
      auto queue_time = queue_map_.find(event_key);
      if (queue_time != queue_map_.end()) {
        context_->slice_tracker->End(timestamp, queue_time->second);
        queue_map_.erase(queue_time);
      }
      last_acquired_[event_key] = timestamp;
      start_slice = false;
      break;
    }
    case GraphicsFrameEvent::LATCH: {
      // b/157578286 - Sometimes Queue event goes missing. To prevent having a
      // wrong slice info, we try to close any existing APP slice.
      auto dequeue_time = dequeue_map_.find(event_key);
      if (dequeue_time != dequeue_map_.end()) {
        InvalidatePhaseEvent(timestamp, dequeue_time->second, true);
        dequeue_map_.erase(dequeue_time);
      }
      track_name.reset();
      track_name.AppendLiteral("SF_");
      track_name.AppendUnsignedInt(buffer_id);
      track_name.AppendLiteral(" ");
      track_name.AppendString(base::StringView(event.layer_name()));
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row sf_track(track_name_id);
      sf_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->LegacyInternGpuTrack(sf_track);
      latch_map_[event_key] = track_id;
      break;
    }

    case GraphicsFrameEvent::PRESENT_FENCE: {
      auto latch_time = latch_map_.find(event_key);
      if (latch_time != latch_map_.end()) {
        context_->slice_tracker->End(timestamp, latch_time->second);
        latch_map_.erase(latch_time);
      }
      auto display_time = display_map_.find(layer_name_id);
      if (display_time != display_map_.end()) {
        context_->slice_tracker->End(timestamp, display_time->second);
        display_map_.erase(display_time);
      }
      base::StringView layerName(event.layer_name());
      track_name.reset();
      track_name.AppendLiteral("Display_");
      track_name.AppendString(layerName.substr(0, 10));
      track_name_id =
          context_->storage->InternString(track_name.GetStringView());
      tables::GpuTrackTable::Row display_track(track_name_id);
      display_track.scope = graphics_event_scope_id_;
      track_id = context_->track_tracker->LegacyInternGpuTrack(display_track);
      display_map_[layer_name_id] = track_id;
      break;
    }

    default:
      start_slice = false;
  }

  // Start the new phase if needed.
  if (start_slice) {
    tables::GraphicsFrameSliceTable::Row slice;
    slice.ts = timestamp;
    slice.track_id = track_id;
    slice.layer_name = layer_name_id;
    slice_name.reset();
    // If the frame_number is known, set it as the name of the slice.
    // If not known (DEQUEUE), set the name as the timestamp.
    // Timestamp is chosen here because the stack_id is hashed based on the name
    // of the slice. To not have any conflicting stack_id with any of the
    // existing slices, we use timestamp as the temporary name.
    if (frame_number != 0) {
      slice_name.AppendUnsignedInt(frame_number);
    } else {
      slice_name.AppendInt(timestamp);
    }
    slice.name = context_->storage->InternString(slice_name.GetStringView());
    slice.frame_number = frame_number;
    context_->slice_tracker->BeginTyped(
        context_->storage->mutable_graphics_frame_slice_table(), slice);
  }
}

void GraphicsFrameEventParser::ParseGraphicsFrameEvent(int64_t timestamp,
                                                       ConstBytes blob) {
  protos::pbzero::GraphicsFrameEvent::Decoder frame_event(blob);
  if (!frame_event.has_buffer_event()) {
    return;
  }

  protos::pbzero::GraphicsFrameEvent::BufferEvent::Decoder event(
      frame_event.buffer_event());
  if (CreateBufferEvent(timestamp, event)) {
    // Create a phase event only if the buffer event finishes successfully
    CreatePhaseEvent(timestamp, event);
  }
}

}  // namespace perfetto::trace_processor
