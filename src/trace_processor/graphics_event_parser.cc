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

#include "src/trace_processor/graphics_event_parser.h"

#include "perfetto/protozero/field.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/track_tracker.h"

#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/android/graphics_frame_event.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_counter_event.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_render_stage_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

GraphicsEventParser::~GraphicsEventParser() = default;

GraphicsEventParser::GraphicsEventParser(TraceProcessorContext* context)
    : context_(context),
      graphics_event_scope_id_(
          context->storage->InternString("graphics_frame_event")),
      unknown_event_name_id_(context->storage->InternString("unknown_event")),
      no_layer_name_name_id_(context->storage->InternString("no_layer_name")),
      layer_name_key_id_(context->storage->InternString("layer_name")),
      event_type_name_ids_{
          {context->storage->InternString("unspecified_event") /* UNSPECIFIED */,
           context->storage->InternString("Dequeue") /* DEQUEUE */,
           context->storage->InternString("Queue") /* QUEUE */,
           context->storage->InternString("Post") /* POST */,
           context->storage->InternString("AcquireFenceSignaled") /* ACQUIRE_FENCE */,
           context->storage->InternString("Latch") /* LATCH */,
           context->storage->InternString("HWCCompositionQueued") /* HWC_COMPOSITION_QUEUED */,
           context->storage->InternString("FallbackComposition") /* FALLBACK_COMPOSITION */,
           context->storage->InternString("PresentFenceSignaled") /* PRESENT_FENCE */,
           context->storage->InternString("ReleaseFenceSignaled") /* RELEASE_FENCE */,
           context->storage->InternString("Modify") /* MODIFY */}} {}

void GraphicsEventParser::ParseGpuCounterEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuCounterEvent::Decoder event(blob.data, blob.size);

  protos::pbzero::GpuCounterDescriptor::Decoder descriptor(
      event.counter_descriptor());
  // Add counter spec to ID map.
  for (auto it = descriptor.specs(); it; ++it) {
    protos::pbzero::GpuCounterDescriptor_GpuCounterSpec::Decoder spec(
        it->data(), it->size());
    if (!spec.has_counter_id()) {
      PERFETTO_ELOG("Counter spec missing counter id");
      context_->storage->IncrementStats(stats::gpu_counters_invalid_spec);
      continue;
    }
    if (!spec.has_name()) {
      context_->storage->IncrementStats(stats::gpu_counters_invalid_spec);
      continue;
    }

    auto counter_id = spec.counter_id();
    auto name = spec.name();
    if (gpu_counter_ids_.find(counter_id) == gpu_counter_ids_.end()) {
      auto desc = spec.description();

      StringId unit_id = 0;
      if (spec.has_numerator_units() || spec.has_denominator_units()) {
        char buffer[1024];
        base::StringWriter unit(buffer, sizeof(buffer));
        for (auto numer = spec.numerator_units(); numer; ++numer) {
          if (unit.pos()) {
            unit.AppendChar(':');
          }
          unit.AppendInt(numer->as_int64());
        }
        char sep = '/';
        for (auto denom = spec.denominator_units(); denom; ++denom) {
          unit.AppendChar(sep);
          unit.AppendInt(denom->as_int64());
          sep = ':';
        }
        unit_id = context_->storage->InternString(unit.GetStringView());
      }

      auto name_id = context_->storage->InternString(name);
      auto desc_id = context_->storage->InternString(desc);
      auto* definitions = context_->storage->mutable_counter_definitions();
      auto defn_id = definitions->AddCounterDefinition(
          name_id, 0, RefType::kRefGpuId, desc_id, unit_id);
      gpu_counter_ids_.emplace(counter_id, defn_id);
    } else {
      // Either counter spec was repeated or it came after counter data.
      PERFETTO_ELOG("Duplicated counter spec found. (counter_id=%d, name=%s)",
                    counter_id, name.ToStdString().c_str());
      context_->storage->IncrementStats(stats::gpu_counters_invalid_spec);
    }
  }

  for (auto it = event.counters(); it; ++it) {
    protos::pbzero::GpuCounterEvent_GpuCounter::Decoder counter(it->data(),
                                                                it->size());
    if (counter.has_counter_id() &&
        (counter.has_int_value() || counter.has_double_value())) {
      auto counter_id = counter.counter_id();
      // Check missing counter_id
      if (gpu_counter_ids_.find(counter_id) == gpu_counter_ids_.end()) {
        char buffer[64];
        base::StringWriter writer(buffer, sizeof(buffer));
        writer.AppendString("gpu_counter(");
        writer.AppendUnsignedInt(counter_id);
        writer.AppendString(")");
        auto name_id = context_->storage->InternString(writer.GetStringView());
        auto* definitions = context_->storage->mutable_counter_definitions();
        auto defn_id =
            definitions->AddCounterDefinition(name_id, 0, RefType::kRefGpuId);
        gpu_counter_ids_.emplace(counter_id, defn_id);
        context_->storage->IncrementStats(stats::gpu_counters_missing_spec);
      }
      if (counter.has_int_value()) {
        context_->event_tracker->PushCounter(ts, counter.int_value(),
                                             gpu_counter_ids_[counter_id]);
      } else {
        context_->event_tracker->PushCounter(ts, counter.double_value(),
                                             gpu_counter_ids_[counter_id]);
      }
    }
  }
}

void GraphicsEventParser::ParseGpuRenderStageEvent(int64_t ts,
                                                   ConstBytes blob) {
  protos::pbzero::GpuRenderStageEvent::Decoder event(blob.data, blob.size);

  if (event.has_specifications()) {
    protos::pbzero::GpuRenderStageEvent_Specifications::Decoder spec(
        event.specifications().data, event.specifications().size);
    for (auto it = spec.hw_queue(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_Specifications_Description::Decoder
          hw_queue(it->data(), it->size());
      if (hw_queue.has_name()) {
        // TODO: create vtrack for each HW queue when it's ready.
        gpu_hw_queue_ids_.emplace_back(
            context_->storage->InternString(hw_queue.name()));
      }
    }
    for (auto it = spec.stage(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_Specifications_Description::Decoder
          stage(it->data(), it->size());
      if (stage.has_name()) {
        gpu_render_stage_ids_.emplace_back(
            context_->storage->InternString(stage.name()));
      }
    }
  }

  auto args_callback = [this, &event](ArgsTracker* args_tracker, RowId row_id) {
    for (auto it = event.extra_data(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_ExtraData_Decoder datum(it->data(),
                                                                  it->size());
      StringId name_id = context_->storage->InternString(datum.name());
      StringId value = context_->storage->InternString(
          datum.has_value() ? datum.value() : base::StringView());
      args_tracker->AddArg(row_id, name_id, name_id, Variadic::String(value));
    }
  };

  if (event.has_event_id()) {
    size_t stage_id = static_cast<size_t>(event.stage_id());
    StringId stage_name;
    if (stage_id < gpu_render_stage_ids_.size()) {
      stage_name = gpu_render_stage_ids_[stage_id];
    } else {
      char buffer[64];
      snprintf(buffer, 64, "render stage(%zu)", stage_id);
      stage_name = context_->storage->InternString(buffer);
    }
    const auto slice_id = context_->slice_tracker->Scoped(
        ts, event.hw_queue_id(), RefType::kRefGpuId, 0, /* cat */
        stage_name, static_cast<int64_t>(event.duration()), args_callback);

    context_->storage->mutable_gpu_slice_table()->Insert(
        tables::GpuSliceTable::Row(
            slice_id.value(), static_cast<int64_t>(event.context()),
            static_cast<int64_t>(event.render_target_handle()),
            base::nullopt /*frame_id*/, event.submission_id(),
            static_cast<uint32_t>(event.hw_queue_id())));
  }
}

void GraphicsEventParser::ParseGraphicsFrameEvent(int64_t timestamp,
                                                  ConstBytes blob) {
  protos::pbzero::GraphicsFrameEvent_Decoder frame_event(blob.data, blob.size);
  if (!frame_event.has_buffer_event()) {
    return;
  }

  ConstBytes bufferBlob = frame_event.buffer_event();
  protos::pbzero::GraphicsFrameEvent_BufferEvent_Decoder event(bufferBlob.data,
                                                               bufferBlob.size);

  if (!event.has_buffer_id()) {
    context_->storage->IncrementStats(
        stats::graphics_frame_event_parser_errors);
    PERFETTO_ELOG("GraphicsFrameEvent with missing buffer id field.");
    return;
  }

  StringId event_name_id = unknown_event_name_id_;
  if (event.has_type()) {
    const auto type = static_cast<size_t>(event.type());
    if (type < event_type_name_ids_.size()) {
      event_name_id = event_type_name_ids_[type];
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

  tables::GpuTrackTable::Row track(track_name_id.id);
  track.scope = graphics_event_scope_id_;
  TrackId track_id = context_->track_tracker->InternGpuTrack(track);

  const auto slice_id = context_->slice_tracker->Scoped(
      timestamp, track_id, RefType::kRefTrack, 0 /* cat */, event_name_id,
      duration, [this, layer_name_id](ArgsTracker* args_tracker, RowId row_id) {
        args_tracker->AddArg(row_id, layer_name_key_id_, layer_name_key_id_,
                             Variadic::String(layer_name_id));
      });

  if (slice_id) {
    tables::GpuSliceTable::Row row;
    row.slice_id = slice_id.value();
    row.frame_id = frame_number;
    context_->storage->mutable_gpu_slice_table()->Insert(row);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
