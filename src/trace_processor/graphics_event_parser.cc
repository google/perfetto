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
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/track_tracker.h"
#include "src/trace_processor/vulkan_memory_tracker.h"

#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/android/graphics_frame_event.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_counter_event.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_log.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_render_stage_event.pbzero.h"
#include "protos/perfetto/trace/gpu/vulkan_memory_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

GraphicsEventParser::~GraphicsEventParser() = default;

GraphicsEventParser::GraphicsEventParser(TraceProcessorContext* context)
    : context_(context),
      gpu_render_stage_scope_id_(
          context->storage->InternString("gpu_render_stage")),
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
           context->storage->InternString("Modify") /* MODIFY */}},
      vulkan_allocated_host_memory_id_(
          context->storage->InternString("vulkan.host.memory")),
      vulkan_allocated_gpu_memory_id_(
          context->storage->InternString("vulkan.gpu.memory")),
      vulkan_live_image_objects_id_(
          context->storage->InternString("vulkan.gpu.images")),
      vulkan_live_buffer_objects_id_(
          context->storage->InternString("vulkan.gpu.buffers")),
      vulkan_bound_image_objects_id_(
          context->storage->InternString("vulkan.gpu.bound_images")),
      vulkan_bound_buffer_objects_id_(
          context->storage->InternString("vulkan.gpu.bound_buffers")),
      vulkan_allocated_host_memory_(0),
      vulkan_allocated_gpu_memory_(0),
      vulkan_live_image_objects_(0),
      vulkan_live_buffer_objects_(0),
      vulkan_bound_image_objects_(0),
      vulkan_bound_buffer_objects_(0),
      gpu_log_track_name_id_(context_->storage->InternString("GPU Log")),
      gpu_log_scope_id_(context_->storage->InternString("gpu_log")),
      tag_id_(context_->storage->InternString("tag")),
      log_message_id_(context->storage->InternString("message")),
      log_severity_ids_{{context_->storage->InternString("UNSPECIFIED"),
                         context_->storage->InternString("VERBOSE"),
                         context_->storage->InternString("DEBUG"),
                         context_->storage->InternString("INFO"),
                         context_->storage->InternString("WARNING"),
                         context_->storage->InternString("ERROR"),
                         context_->storage->InternString(
                             "UNKNOWN_SEVERITY") /* must be last */}} {}

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
        StringId track_name = context_->storage->InternString(hw_queue.name());
        tables::GpuTrackTable::Row track(track_name.id);
        track.scope = gpu_render_stage_scope_id_;
        gpu_hw_queue_ids_.emplace_back(
            context_->track_tracker->InternGpuTrack(track));
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
    TrackId track_id =
        gpu_hw_queue_ids_[static_cast<size_t>(event.hw_queue_id())];
    const auto slice_id = context_->slice_tracker->Scoped(
        ts, track_id, track_id, RefType::kRefTrack, 0 /* cat */, stage_name,
        static_cast<int64_t>(event.duration()), args_callback);

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
      timestamp, track_id, track_id, RefType::kRefTrack, 0 /* cat */,
      event_name_id, duration,
      [this, layer_name_id](ArgsTracker* args_tracker, RowId row_id) {
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

void GraphicsEventParser::UpdateVulkanMemoryAllocationCounters(
    const tables::VulkanMemoryAllocationsTable::Row* row) {
  auto ts = row->timestamp;

  if (row->source_iid == protos::pbzero::VulkanMemoryEvent_Source_HOST) {
    if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_CREATE) {
      vulkan_allocated_host_memory_ += row->memory_size.value();
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY) {
      vulkan_allocated_host_memory_ -= row->memory_size.value();
    }
    context_->event_tracker->PushCounter(ts, vulkan_allocated_host_memory_,
                                         vulkan_allocated_host_memory_id_, 0,
                                         RefType::kRefNoRef);
  } else if (row->source_iid ==
             protos::pbzero::VulkanMemoryEvent_Source_GPU_DEVICE_MEMORY) {
    if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_CREATE) {
      vulkan_allocated_gpu_memory_ += row->memory_size.value();
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY) {
      vulkan_allocated_gpu_memory_ -= row->memory_size.value();
    }
    context_->event_tracker->PushCounter(ts, vulkan_allocated_gpu_memory_,
                                         vulkan_allocated_gpu_memory_id_, 0,
                                         RefType::kRefNoRef);
  } else if (row->source_iid ==
             protos::pbzero::VulkanMemoryEvent_Source_GPU_BUFFER) {
    if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_CREATE) {
      vulkan_live_buffer_objects_ += 1;
      context_->event_tracker->PushCounter(ts, vulkan_live_buffer_objects_,
                                           vulkan_live_buffer_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY) {
      vulkan_live_buffer_objects_ -= 1;
      context_->event_tracker->PushCounter(ts, vulkan_live_buffer_objects_,
                                           vulkan_live_buffer_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_BIND) {
      vulkan_bound_buffer_objects_ += 1;
      context_->event_tracker->PushCounter(ts, vulkan_bound_buffer_objects_,
                                           vulkan_bound_buffer_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY_BOUND) {
      vulkan_bound_buffer_objects_ -= 1;
      context_->event_tracker->PushCounter(ts, vulkan_bound_buffer_objects_,
                                           vulkan_bound_buffer_objects_id_, 0,
                                           RefType::kRefNoRef);
    }
  } else if (row->source_iid ==
             protos::pbzero::VulkanMemoryEvent_Source_GPU_IMAGE) {
    if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_CREATE) {
      vulkan_live_image_objects_ += 1;
      context_->event_tracker->PushCounter(ts, vulkan_live_image_objects_,
                                           vulkan_live_image_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY) {
      vulkan_live_image_objects_ -= 1;
      context_->event_tracker->PushCounter(ts, vulkan_live_image_objects_,
                                           vulkan_live_image_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid == protos::pbzero::VulkanMemoryEvent_Type_BIND) {
      vulkan_bound_image_objects_ += 1;
      context_->event_tracker->PushCounter(ts, vulkan_bound_image_objects_,
                                           vulkan_bound_image_objects_id_, 0,
                                           RefType::kRefNoRef);
    } else if (row->type_iid ==
               protos::pbzero::VulkanMemoryEvent_Type_DESTROY_BOUND) {
      vulkan_bound_image_objects_ -= 1;
      context_->event_tracker->PushCounter(ts, vulkan_bound_image_objects_,
                                           vulkan_bound_image_objects_id_, 0,
                                           RefType::kRefNoRef);
    }
  }
}

void GraphicsEventParser::ParseVulkanMemoryEvent(ConstBytes blob) {
  protos::pbzero::VulkanMemoryEvent::Decoder vulkan_memory_event(blob.data,
                                                                 blob.size);

  tables::VulkanMemoryAllocationsTable::Row vulkan_memory_event_row;
  if (vulkan_memory_event.has_source()) {
    vulkan_memory_event_row.source_iid =
        *(context_->vulkan_memory_tracker->FindSourceString(
            static_cast<uint64_t>(vulkan_memory_event.source())));
  }
  if (vulkan_memory_event.has_type()) {
    vulkan_memory_event_row.type_iid =
        *(context_->vulkan_memory_tracker->FindTypeString(
            static_cast<uint64_t>(vulkan_memory_event.type())));
  }
  if (vulkan_memory_event.has_timestamp())
    vulkan_memory_event_row.timestamp = vulkan_memory_event.timestamp();
  if (vulkan_memory_event.has_pid()) {
    vulkan_memory_event_row.upid =
        context_->process_tracker->GetOrCreateProcess(
            vulkan_memory_event.pid());
  }
  if (vulkan_memory_event.has_device())
    vulkan_memory_event_row.device =
        static_cast<int64_t>(vulkan_memory_event.device());
  if (vulkan_memory_event.has_device_memory())
    vulkan_memory_event_row.device_memory =
        static_cast<int64_t>(vulkan_memory_event.device_memory());
  if (vulkan_memory_event.has_heap())
    vulkan_memory_event_row.heap = vulkan_memory_event.heap();
  if (vulkan_memory_event.has_caller_iid()) {
    vulkan_memory_event_row.caller_iid =
        *(context_->vulkan_memory_tracker->FindString(
            static_cast<uint64_t>(vulkan_memory_event.caller_iid())));
  }
  if (vulkan_memory_event.has_object_handle())
    vulkan_memory_event_row.object_handle =
        static_cast<int64_t>(vulkan_memory_event.object_handle());
  if (vulkan_memory_event.has_memory_address())
    vulkan_memory_event_row.memory_address =
        static_cast<int64_t>(vulkan_memory_event.memory_address());
  if (vulkan_memory_event.has_memory_size())
    vulkan_memory_event_row.memory_size =
        static_cast<int64_t>(vulkan_memory_event.memory_size());

  UpdateVulkanMemoryAllocationCounters(&vulkan_memory_event_row);

  auto row_id =
      context_->storage->mutable_vulkan_memory_allocations_table()->Insert(
          vulkan_memory_event_row);

  if (vulkan_memory_event.has_annotations()) {
    auto global_row_id =
        TraceStorage::CreateRowId(TableId::kVulkanMemoryAllocation, row_id);
    for (auto itt = vulkan_memory_event.annotations(); itt; ++itt) {
      protos::pbzero::VulkanMemoryEventAnnotation::Decoder annotation(
          itt->data(), itt->size());
      auto annotation_id =
          *(context_->vulkan_memory_tracker->FindString(annotation.key_iid()));
      if (annotation.has_int_value()) {
        context_->args_tracker->AddArg(
            global_row_id, annotation_id, annotation_id,
            Variadic::Integer(annotation.int_value()));

      } else if (annotation.has_double_value()) {
        context_->args_tracker->AddArg(
            global_row_id, annotation_id, annotation_id,
            Variadic::Real(annotation.double_value()));

      } else if (annotation.has_string_iid()) {
        context_->args_tracker->AddArg(
            global_row_id, annotation_id, annotation_id,
            Variadic::String(*(context_->vulkan_memory_tracker->FindString(
                annotation.string_iid()))));
      }
    }
  }
}

void GraphicsEventParser::ParseGpuLog(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuLog::Decoder event(blob.data, blob.size);

  tables::GpuTrackTable::Row track(gpu_log_track_name_id_.id);
  track.scope = gpu_log_scope_id_;
  TrackId track_id = context_->track_tracker->InternGpuTrack(track);

  auto args_callback = [this, &event](ArgsTracker* args_tracker, RowId row_id) {
    if (event.has_tag()) {
      args_tracker->AddArg(
          row_id, tag_id_, tag_id_,
          Variadic::String(context_->storage->InternString(event.tag())));
    }
    if (event.has_log_message()) {
      args_tracker->AddArg(row_id, log_message_id_, log_message_id_,
                           Variadic::String(context_->storage->InternString(
                               event.log_message())));
    }
  };

  auto severity = static_cast<size_t>(event.severity());
  StringId severity_id =
      severity < log_severity_ids_.size()
          ? log_severity_ids_[static_cast<size_t>(event.severity())]
          : log_severity_ids_[log_severity_ids_.size() - 1];
  const auto slice_id = context_->slice_tracker->Scoped(
      ts, track_id, track_id, RefType::kRefTrack, 0 /* cat */, severity_id,
      0 /* duration */, args_callback);

  tables::GpuSliceTable::Row row;
  row.slice_id = slice_id.value();
  context_->storage->mutable_gpu_slice_table()->Insert(row);
}

}  // namespace trace_processor
}  // namespace perfetto
