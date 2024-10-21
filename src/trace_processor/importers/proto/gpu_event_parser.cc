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

#include "src/trace_processor/importers/proto/gpu_event_parser.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/string_writer.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/gpu_mem_event.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/vulkan_memory_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_counter_event.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_log.pbzero.h"
#include "protos/perfetto/trace/gpu/gpu_render_stage_event.pbzero.h"
#include "protos/perfetto/trace/gpu/vulkan_api_event.pbzero.h"
#include "protos/perfetto/trace/gpu/vulkan_memory_event.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

namespace {

// https://www.khronos.org/registry/vulkan/specs/1.1-extensions/man/html/VkObjectType.html
enum VkObjectType {
  VK_OBJECT_TYPE_UNKNOWN = 0,
  VK_OBJECT_TYPE_INSTANCE = 1,
  VK_OBJECT_TYPE_PHYSICAL_DEVICE = 2,
  VK_OBJECT_TYPE_DEVICE = 3,
  VK_OBJECT_TYPE_QUEUE = 4,
  VK_OBJECT_TYPE_SEMAPHORE = 5,
  VK_OBJECT_TYPE_COMMAND_BUFFER = 6,
  VK_OBJECT_TYPE_FENCE = 7,
  VK_OBJECT_TYPE_DEVICE_MEMORY = 8,
  VK_OBJECT_TYPE_BUFFER = 9,
  VK_OBJECT_TYPE_IMAGE = 10,
  VK_OBJECT_TYPE_EVENT = 11,
  VK_OBJECT_TYPE_QUERY_POOL = 12,
  VK_OBJECT_TYPE_BUFFER_VIEW = 13,
  VK_OBJECT_TYPE_IMAGE_VIEW = 14,
  VK_OBJECT_TYPE_SHADER_MODULE = 15,
  VK_OBJECT_TYPE_PIPELINE_CACHE = 16,
  VK_OBJECT_TYPE_PIPELINE_LAYOUT = 17,
  VK_OBJECT_TYPE_RENDER_PASS = 18,
  VK_OBJECT_TYPE_PIPELINE = 19,
  VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT = 20,
  VK_OBJECT_TYPE_SAMPLER = 21,
  VK_OBJECT_TYPE_DESCRIPTOR_POOL = 22,
  VK_OBJECT_TYPE_DESCRIPTOR_SET = 23,
  VK_OBJECT_TYPE_FRAMEBUFFER = 24,
  VK_OBJECT_TYPE_COMMAND_POOL = 25,
  VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION = 1000156000,
  VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE = 1000085000,
  VK_OBJECT_TYPE_SURFACE_KHR = 1000000000,
  VK_OBJECT_TYPE_SWAPCHAIN_KHR = 1000001000,
  VK_OBJECT_TYPE_DISPLAY_KHR = 1000002000,
  VK_OBJECT_TYPE_DISPLAY_MODE_KHR = 1000002001,
  VK_OBJECT_TYPE_DEBUG_REPORT_CALLBACK_EXT = 1000011000,
  VK_OBJECT_TYPE_OBJECT_TABLE_NVX = 1000086000,
  VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NVX = 1000086001,
  VK_OBJECT_TYPE_DEBUG_UTILS_MESSENGER_EXT = 1000128000,
  VK_OBJECT_TYPE_VALIDATION_CACHE_EXT = 1000160000,
  VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_NV = 1000165000,
  VK_OBJECT_TYPE_PERFORMANCE_CONFIGURATION_INTEL = 1000210000,
  VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_KHR =
      VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
  VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION_KHR =
      VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
  VK_OBJECT_TYPE_MAX_ENUM = 0x7FFFFFFF
};

using protos::pbzero::VulkanMemoryEvent;

}  // anonymous namespace

GpuEventParser::GpuEventParser(TraceProcessorContext* context)
    : context_(context),
      vulkan_memory_tracker_(context),
      description_id_(context->storage->InternString("description")),
      gpu_render_stage_scope_id_(
          context->storage->InternString("gpu_render_stage")),
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
                             "UNKNOWN_SEVERITY") /* must be last */}},
      vk_event_track_id_(context->storage->InternString("Vulkan Events")),
      vk_event_scope_id_(context->storage->InternString("vulkan_events")),
      vk_queue_submit_id_(context->storage->InternString("vkQueueSubmit")),
      gpu_mem_total_name_id_(context->storage->InternString("GPU Memory")),
      gpu_mem_total_unit_id_(context->storage->InternString(
          std::to_string(protos::pbzero::GpuCounterDescriptor::BYTE).c_str())),
      gpu_mem_total_global_desc_id_(context->storage->InternString(
          "Total GPU memory used by the entire system")),
      gpu_mem_total_proc_desc_id_(context->storage->InternString(
          "Total GPU memory used by this process")) {}

void GpuEventParser::ParseGpuCounterEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuCounterEvent::Decoder event(blob.data, blob.size);

  protos::pbzero::GpuCounterDescriptor::Decoder descriptor(
      event.counter_descriptor());
  // Add counter spec to ID map.
  for (auto it = descriptor.specs(); it; ++it) {
    protos::pbzero::GpuCounterDescriptor_GpuCounterSpec::Decoder spec(*it);
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
    if (gpu_counter_track_ids_.find(counter_id) ==
        gpu_counter_track_ids_.end()) {
      auto desc = spec.description();

      StringId unit_id = kNullStringId;
      if (spec.has_numerator_units() || spec.has_denominator_units()) {
        char buffer[1024];
        base::StringWriter unit(buffer, sizeof(buffer));
        for (auto numer = spec.numerator_units(); numer; ++numer) {
          if (unit.pos()) {
            unit.AppendChar(':');
          }
          unit.AppendInt(*numer);
        }
        char sep = '/';
        for (auto denom = spec.denominator_units(); denom; ++denom) {
          unit.AppendChar(sep);
          unit.AppendInt(*denom);
          sep = ':';
        }
        unit_id = context_->storage->InternString(unit.GetStringView());
      }

      auto name_id = context_->storage->InternString(name);
      auto desc_id = context_->storage->InternString(desc);
      auto track_id = context_->track_tracker->LegacyCreateGpuCounterTrack(
          name_id, 0 /* gpu_id */, desc_id, unit_id);
      gpu_counter_track_ids_.emplace(counter_id, track_id);
      if (spec.has_groups()) {
        for (auto group = spec.groups(); group; ++group) {
          tables::GpuCounterGroupTable::Row row;
          row.group_id = *group;
          row.track_id = track_id;
          context_->storage->mutable_gpu_counter_group_table()->Insert(row);
        }
      } else {
        tables::GpuCounterGroupTable::Row row;
        row.group_id = protos::pbzero::GpuCounterDescriptor::UNCLASSIFIED;
        row.track_id = track_id;
        context_->storage->mutable_gpu_counter_group_table()->Insert(row);
      }
    } else {
      // Either counter spec was repeated or it came after counter data.
      PERFETTO_ELOG("Duplicated counter spec found. (counter_id=%d, name=%s)",
                    counter_id, name.ToStdString().c_str());
      context_->storage->IncrementStats(stats::gpu_counters_invalid_spec);
    }
  }

  for (auto it = event.counters(); it; ++it) {
    protos::pbzero::GpuCounterEvent_GpuCounter::Decoder counter(*it);
    if (counter.has_counter_id() &&
        (counter.has_int_value() || counter.has_double_value())) {
      auto counter_id = counter.counter_id();
      // Check missing counter_id
      if (gpu_counter_track_ids_.find(counter_id) ==
          gpu_counter_track_ids_.end()) {
        continue;
      }
      double counter_val = counter.has_int_value()
                               ? static_cast<double>(counter.int_value())
                               : counter.double_value();
      context_->event_tracker->PushCounter(ts, counter_val,
                                           gpu_counter_track_ids_[counter_id]);
    }
  }
}

StringId GpuEventParser::GetFullStageName(
    PacketSequenceStateGeneration* sequence_state,
    const protos::pbzero::GpuRenderStageEvent_Decoder& event) const {
  StringId stage_name;
  if (event.has_stage_iid()) {
    auto stage_iid = event.stage_iid();
    auto* decoder = sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kGpuSpecificationsFieldNumber,
        protos::pbzero::InternedGpuRenderStageSpecification>(stage_iid);
    if (!decoder) {
      return kNullStringId;
    }
    stage_name = context_->storage->InternString(decoder->name());
  } else {
    auto stage_id = static_cast<uint64_t>(event.stage_id());
    if (stage_id < gpu_render_stage_ids_.size()) {
      stage_name = gpu_render_stage_ids_[static_cast<size_t>(stage_id)].first;
    } else {
      base::StackString<64> name("render stage(%" PRIu64 ")", stage_id);
      stage_name = context_->storage->InternString(name.string_view());
    }
  }
  return stage_name;
}

/**
 * Create a GPU render stage track based
 * GpuRenderStageEvent.Specifications.Description.
 */
void GpuEventParser::InsertGpuTrack(
    const protos::pbzero::
        GpuRenderStageEvent_Specifications_Description_Decoder& hw_queue) {
  StringId track_name = context_->storage->InternString(hw_queue.name());
  if (gpu_hw_queue_counter_ >= gpu_hw_queue_ids_.size() ||
      !gpu_hw_queue_ids_[gpu_hw_queue_counter_].has_value()) {
    tables::GpuTrackTable::Row track(track_name);
    track.scope = gpu_render_stage_scope_id_;
    track.description = context_->storage->InternString(hw_queue.description());
    if (gpu_hw_queue_counter_ >= gpu_hw_queue_ids_.size()) {
      gpu_hw_queue_ids_.emplace_back(
          context_->track_tracker->InternGpuTrack(track));
    } else {
      // If a gpu_render_stage_event is received before the specification, it is
      // possible that the slot has already been allocated.
      gpu_hw_queue_ids_[gpu_hw_queue_counter_] =
          context_->track_tracker->InternGpuTrack(track);
    }
  } else {
    // If a gpu_render_stage_event is received before the specification, a track
    // will be automatically generated.  In that case, update the name and
    // description.
    auto track_id = gpu_hw_queue_ids_[gpu_hw_queue_counter_];
    if (track_id.has_value()) {
      auto rr = *context_->storage->mutable_gpu_track_table()->FindById(
          track_id.value());
      rr.set_name(track_name);
      rr.set_description(
          context_->storage->InternString(hw_queue.description()));
    } else {
      tables::GpuTrackTable::Row track(track_name);
      track.scope = gpu_render_stage_scope_id_;
      track.description =
          context_->storage->InternString(hw_queue.description());
    }
  }
  ++gpu_hw_queue_counter_;
}
std::optional<std::string> GpuEventParser::FindDebugName(
    int32_t vk_object_type,
    uint64_t vk_handle) const {
  auto map = debug_marker_names_.find(vk_object_type);
  if (map == debug_marker_names_.end()) {
    return std::nullopt;
  }

  auto name = map->second.find(vk_handle);
  if (name == map->second.end()) {
    return std::nullopt;
  }
  return name->second;
}

StringId GpuEventParser::ParseRenderSubpasses(
    const protos::pbzero::GpuRenderStageEvent_Decoder& event) const {
  if (!event.has_render_subpass_index_mask()) {
    return kNullStringId;
  }
  char buf[256];
  base::StringWriter writer(buf, sizeof(buf));
  uint32_t bit_index = 0;
  bool first = true;
  for (auto it = event.render_subpass_index_mask(); it; ++it) {
    auto subpasses_bits = *it;
    do {
      if ((subpasses_bits & 1) != 0) {
        if (!first) {
          writer.AppendChar(',');
        }
        first = false;
        writer.AppendUnsignedInt(bit_index);
      }
      subpasses_bits >>= 1;
      ++bit_index;
    } while (subpasses_bits != 0);
    // Round up to the next multiple of 64.
    bit_index = ((bit_index - 1) / 64 + 1) * 64;
  }
  return context_->storage->InternString(writer.GetStringView());
}

void GpuEventParser::ParseGpuRenderStageEvent(
    int64_t ts,
    PacketSequenceStateGeneration* sequence_state,
    ConstBytes blob) {
  protos::pbzero::GpuRenderStageEvent::Decoder event(blob.data, blob.size);

  int32_t pid = 0;
  if (event.has_specifications()) {
    protos::pbzero::GpuRenderStageEvent_Specifications::Decoder spec(
        event.specifications().data, event.specifications().size);
    for (auto it = spec.hw_queue(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_Specifications_Description::Decoder
          hw_queue(*it);
      if (hw_queue.has_name()) {
        InsertGpuTrack(hw_queue);
      }
    }
    for (auto it = spec.stage(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_Specifications_Description::Decoder
          stage(*it);
      if (stage.has_name()) {
        gpu_render_stage_ids_.emplace_back(
            context_->storage->InternString(stage.name()),
            context_->storage->InternString(stage.description()));
      }
    }
    if (spec.has_context_spec()) {
      protos::pbzero::GpuRenderStageEvent_Specifications_ContextSpec::Decoder
          context_spec(spec.context_spec());
      if (context_spec.has_pid()) {
        pid = context_spec.pid();
      }
    }
  }

  if (event.has_context()) {
    uint64_t context_id = event.context();
    auto* decoder = sequence_state->LookupInternedMessage<
        protos::pbzero::InternedData::kGraphicsContextsFieldNumber,
        protos::pbzero::InternedGraphicsContext>(context_id);
    if (decoder) {
      pid = decoder->pid();
    }
  }

  auto args_callback = [this, &event,
                        sequence_state](ArgsTracker::BoundInserter* inserter) {
    if (event.has_stage_iid()) {
      size_t stage_iid = static_cast<size_t>(event.stage_iid());
      auto* decoder = sequence_state->LookupInternedMessage<
          protos::pbzero::InternedData::kGpuSpecificationsFieldNumber,
          protos::pbzero::InternedGpuRenderStageSpecification>(stage_iid);
      if (decoder) {
        // TODO: Add RenderStageCategory to gpu_slice table.
        inserter->AddArg(description_id_,
                         Variadic::String(context_->storage->InternString(
                             decoder->description())));
      }
    } else if (event.has_stage_id()) {
      size_t stage_id = static_cast<size_t>(event.stage_id());
      if (stage_id < gpu_render_stage_ids_.size()) {
        auto description = gpu_render_stage_ids_[stage_id].second;
        if (description != kNullStringId) {
          inserter->AddArg(description_id_, Variadic::String(description));
        }
      }
    }
    for (auto it = event.extra_data(); it; ++it) {
      protos::pbzero::GpuRenderStageEvent_ExtraData_Decoder datum(*it);
      StringId name_id = context_->storage->InternString(datum.name());
      StringId value = context_->storage->InternString(
          datum.has_value() ? datum.value() : base::StringView());
      inserter->AddArg(name_id, Variadic::String(value));
    }
  };

  if (event.has_event_id()) {
    TrackId track_id;
    uint64_t hw_queue_id = 0;
    if (event.has_hw_queue_iid()) {
      hw_queue_id = event.hw_queue_iid();
      auto* decoder = sequence_state->LookupInternedMessage<
          protos::pbzero::InternedData::kGpuSpecificationsFieldNumber,
          protos::pbzero::InternedGpuRenderStageSpecification>(hw_queue_id);
      if (!decoder) {
        // Skip
        return;
      }
      // TODO: Add RenderStageCategory to gpu_track table.
      tables::GpuTrackTable::Row track(
          context_->storage->InternString(decoder->name()));
      track.scope = gpu_render_stage_scope_id_;
      track.description =
          context_->storage->InternString(decoder->description());
      track_id = context_->track_tracker->InternGpuTrack(track);
    } else {
      uint32_t id = static_cast<uint32_t>(event.hw_queue_id());
      if (id < gpu_hw_queue_ids_.size() && gpu_hw_queue_ids_[id].has_value()) {
        track_id = gpu_hw_queue_ids_[id].value();
      } else {
        // If the event has a hw_queue_id that does not have a Specification,
        // create a new track for it.
        char buf[128];
        base::StringWriter writer(buf, sizeof(buf));
        writer.AppendLiteral("Unknown GPU Queue ");
        if (id > 1024) {
          // We don't expect this to happen, but just in case there is a corrupt
          // packet, make sure we don't allocate a ridiculous amount of memory.
          id = 1024;
          context_->storage->IncrementStats(
              stats::gpu_render_stage_parser_errors);
          PERFETTO_ELOG("Invalid hw_queue_id.");
        } else {
          writer.AppendInt(event.hw_queue_id());
        }
        StringId track_name =
            context_->storage->InternString(writer.GetStringView());
        tables::GpuTrackTable::Row track(track_name);
        track.scope = gpu_render_stage_scope_id_;
        track_id = context_->track_tracker->InternGpuTrack(track);
        gpu_hw_queue_ids_.resize(id + 1);
        gpu_hw_queue_ids_[id] = track_id;
      }
      hw_queue_id = id;
    }

    auto render_target_name =
        FindDebugName(VK_OBJECT_TYPE_FRAMEBUFFER, event.render_target_handle());
    auto render_target_name_id = render_target_name.has_value()
                                     ? context_->storage->InternString(
                                           render_target_name.value().c_str())
                                     : kNullStringId;
    auto render_pass_name =
        FindDebugName(VK_OBJECT_TYPE_RENDER_PASS, event.render_pass_handle());
    auto render_pass_name_id =
        render_pass_name.has_value()
            ? context_->storage->InternString(render_pass_name.value().c_str())
            : kNullStringId;
    auto command_buffer_name = FindDebugName(VK_OBJECT_TYPE_COMMAND_BUFFER,
                                             event.command_buffer_handle());
    auto command_buffer_name_id = command_buffer_name.has_value()
                                      ? context_->storage->InternString(
                                            command_buffer_name.value().c_str())
                                      : kNullStringId;

    tables::GpuSliceTable::Row row;
    row.ts = ts;
    row.track_id = track_id;
    row.name = GetFullStageName(sequence_state, event);
    row.dur = static_cast<int64_t>(event.duration());
    // TODO: Create table for graphics context and lookup
    // InternedGraphicsContext.
    row.context_id = static_cast<int64_t>(event.context());
    row.render_target = static_cast<int64_t>(event.render_target_handle());
    row.render_target_name = render_target_name_id;
    row.render_pass = static_cast<int64_t>(event.render_pass_handle());
    row.render_pass_name = render_pass_name_id;
    row.render_subpasses = ParseRenderSubpasses(event);
    row.command_buffer = static_cast<int64_t>(event.command_buffer_handle());
    row.command_buffer_name = command_buffer_name_id;
    row.submission_id = event.submission_id();
    row.hw_queue_id = static_cast<int64_t>(hw_queue_id);
    row.upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(pid));
    context_->slice_tracker->ScopedTyped(
        context_->storage->mutable_gpu_slice_table(), row, args_callback);
  }
}

void GpuEventParser::UpdateVulkanMemoryAllocationCounters(
    UniquePid upid,
    const VulkanMemoryEvent::Decoder& event) {
  StringId track_str_id = kNullStringId;
  TrackId track = kInvalidTrackId;
  auto allocation_scope = VulkanMemoryEvent::SCOPE_UNSPECIFIED;
  uint32_t memory_type = std::numeric_limits<uint32_t>::max();
  switch (event.source()) {
    case VulkanMemoryEvent::SOURCE_DRIVER:
      allocation_scope = static_cast<VulkanMemoryEvent::AllocationScope>(
          event.allocation_scope());
      if (allocation_scope == VulkanMemoryEvent::SCOPE_UNSPECIFIED)
        return;
      switch (event.operation()) {
        case VulkanMemoryEvent::OP_CREATE:
          vulkan_driver_memory_counters_[allocation_scope] +=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_DESTROY:
          vulkan_driver_memory_counters_[allocation_scope] -=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_UNSPECIFIED:
        case VulkanMemoryEvent::OP_BIND:
        case VulkanMemoryEvent::OP_DESTROY_BOUND:
        case VulkanMemoryEvent::OP_ANNOTATIONS:
          return;
      }
      track_str_id = vulkan_memory_tracker_.FindAllocationScopeCounterString(
          allocation_scope);
      track = context_->track_tracker->InternProcessCounterTrack(track_str_id,
                                                                 upid);
      context_->event_tracker->PushCounter(
          event.timestamp(),
          static_cast<double>(vulkan_driver_memory_counters_[allocation_scope]),
          track);
      break;

    case VulkanMemoryEvent::SOURCE_DEVICE_MEMORY:
      memory_type = static_cast<uint32_t>(event.memory_type());
      switch (event.operation()) {
        case VulkanMemoryEvent::OP_CREATE:
          vulkan_device_memory_counters_allocate_[memory_type] +=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_DESTROY:
          vulkan_device_memory_counters_allocate_[memory_type] -=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_UNSPECIFIED:
        case VulkanMemoryEvent::OP_BIND:
        case VulkanMemoryEvent::OP_DESTROY_BOUND:
        case VulkanMemoryEvent::OP_ANNOTATIONS:
          return;
      }
      track_str_id = vulkan_memory_tracker_.FindMemoryTypeCounterString(
          memory_type,
          VulkanMemoryTracker::DeviceCounterType::kAllocationCounter);
      track = context_->track_tracker->InternProcessCounterTrack(track_str_id,
                                                                 upid);
      context_->event_tracker->PushCounter(
          event.timestamp(),
          static_cast<double>(
              vulkan_device_memory_counters_allocate_[memory_type]),
          track);
      break;

    case VulkanMemoryEvent::SOURCE_BUFFER:
    case VulkanMemoryEvent::SOURCE_IMAGE:
      memory_type = static_cast<uint32_t>(event.memory_type());
      switch (event.operation()) {
        case VulkanMemoryEvent::OP_BIND:
          vulkan_device_memory_counters_bind_[memory_type] +=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_DESTROY_BOUND:
          vulkan_device_memory_counters_bind_[memory_type] -=
              event.memory_size();
          break;
        case VulkanMemoryEvent::OP_UNSPECIFIED:
        case VulkanMemoryEvent::OP_CREATE:
        case VulkanMemoryEvent::OP_DESTROY:
        case VulkanMemoryEvent::OP_ANNOTATIONS:
          return;
      }
      track_str_id = vulkan_memory_tracker_.FindMemoryTypeCounterString(
          memory_type, VulkanMemoryTracker::DeviceCounterType::kBindCounter);
      track = context_->track_tracker->InternProcessCounterTrack(track_str_id,
                                                                 upid);
      context_->event_tracker->PushCounter(
          event.timestamp(),
          static_cast<double>(vulkan_device_memory_counters_bind_[memory_type]),
          track);
      break;
    case VulkanMemoryEvent::SOURCE_UNSPECIFIED:
    case VulkanMemoryEvent::SOURCE_DEVICE:
      return;
  }
}

void GpuEventParser::ParseVulkanMemoryEvent(
    PacketSequenceStateGeneration* sequence_state,
    ConstBytes blob) {
  using protos::pbzero::InternedData;
  VulkanMemoryEvent::Decoder vulkan_memory_event(blob.data, blob.size);
  tables::VulkanMemoryAllocationsTable::Row vulkan_memory_event_row;
  vulkan_memory_event_row.source = vulkan_memory_tracker_.FindSourceString(
      static_cast<VulkanMemoryEvent::Source>(vulkan_memory_event.source()));
  vulkan_memory_event_row.operation =
      vulkan_memory_tracker_.FindOperationString(
          static_cast<VulkanMemoryEvent::Operation>(
              vulkan_memory_event.operation()));
  vulkan_memory_event_row.timestamp = vulkan_memory_event.timestamp();
  vulkan_memory_event_row.upid =
      context_->process_tracker->GetOrCreateProcess(vulkan_memory_event.pid());
  if (vulkan_memory_event.has_device())
    vulkan_memory_event_row.device =
        static_cast<int64_t>(vulkan_memory_event.device());
  if (vulkan_memory_event.has_device_memory())
    vulkan_memory_event_row.device_memory =
        static_cast<int64_t>(vulkan_memory_event.device_memory());
  if (vulkan_memory_event.has_heap())
    vulkan_memory_event_row.heap = vulkan_memory_event.heap();
  if (vulkan_memory_event.has_memory_type())
    vulkan_memory_event_row.memory_type = vulkan_memory_event.memory_type();
  if (vulkan_memory_event.has_caller_iid()) {
    vulkan_memory_event_row.function_name =
        vulkan_memory_tracker_
            .GetInternedString<InternedData::kFunctionNamesFieldNumber>(
                sequence_state,
                static_cast<uint64_t>(vulkan_memory_event.caller_iid()));
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
  if (vulkan_memory_event.has_allocation_scope())
    vulkan_memory_event_row.scope =
        vulkan_memory_tracker_.FindAllocationScopeString(
            static_cast<VulkanMemoryEvent::AllocationScope>(
                vulkan_memory_event.allocation_scope()));

  UpdateVulkanMemoryAllocationCounters(vulkan_memory_event_row.upid.value(),
                                       vulkan_memory_event);

  auto* allocs = context_->storage->mutable_vulkan_memory_allocations_table();
  VulkanAllocId id = allocs->Insert(vulkan_memory_event_row).id;

  if (vulkan_memory_event.has_annotations()) {
    auto inserter = context_->args_tracker->AddArgsTo(id);

    for (auto it = vulkan_memory_event.annotations(); it; ++it) {
      protos::pbzero::VulkanMemoryEventAnnotation::Decoder annotation(*it);

      auto key_id =
          vulkan_memory_tracker_
              .GetInternedString<InternedData::kVulkanMemoryKeysFieldNumber>(
                  sequence_state, static_cast<uint64_t>(annotation.key_iid()));

      if (annotation.has_int_value()) {
        inserter.AddArg(key_id, Variadic::Integer(annotation.int_value()));
      } else if (annotation.has_double_value()) {
        inserter.AddArg(key_id, Variadic::Real(annotation.double_value()));
      } else if (annotation.has_string_iid()) {
        auto string_id =
            vulkan_memory_tracker_
                .GetInternedString<InternedData::kVulkanMemoryKeysFieldNumber>(
                    sequence_state,
                    static_cast<uint64_t>(annotation.string_iid()));

        inserter.AddArg(key_id, Variadic::String(string_id));
      }
    }
  }
}

void GpuEventParser::ParseGpuLog(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuLog::Decoder event(blob.data, blob.size);

  tables::GpuTrackTable::Row track(gpu_log_track_name_id_);
  track.scope = gpu_log_scope_id_;
  TrackId track_id = context_->track_tracker->InternGpuTrack(track);

  auto args_callback = [this, &event](ArgsTracker::BoundInserter* inserter) {
    if (event.has_tag()) {
      inserter->AddArg(
          tag_id_,
          Variadic::String(context_->storage->InternString(event.tag())));
    }
    if (event.has_log_message()) {
      inserter->AddArg(log_message_id_,
                       Variadic::String(context_->storage->InternString(
                           event.log_message())));
    }
  };

  auto severity = static_cast<size_t>(event.severity());
  StringId severity_id =
      severity < log_severity_ids_.size()
          ? log_severity_ids_[static_cast<size_t>(event.severity())]
          : log_severity_ids_[log_severity_ids_.size() - 1];

  tables::GpuSliceTable::Row row;
  row.ts = ts;
  row.track_id = track_id;
  row.name = severity_id;
  row.dur = 0;
  context_->slice_tracker->ScopedTyped(
      context_->storage->mutable_gpu_slice_table(), row, args_callback);
}

void GpuEventParser::ParseVulkanApiEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::VulkanApiEvent::Decoder vk_event(blob.data, blob.size);
  if (vk_event.has_vk_debug_utils_object_name()) {
    protos::pbzero::VulkanApiEvent_VkDebugUtilsObjectName::Decoder event(
        vk_event.vk_debug_utils_object_name());
    debug_marker_names_[event.object_type()][event.object()] =
        event.object_name().ToStdString();
  }
  if (vk_event.has_vk_queue_submit()) {
    protos::pbzero::VulkanApiEvent_VkQueueSubmit::Decoder event(
        vk_event.vk_queue_submit());
    // Once flow table is implemented, we can create a nice UI that link the
    // vkQueueSubmit to GpuRenderStageEvent.  For now, just add it as in a GPU
    // track so that they can appear close to the render stage slices.
    tables::GpuTrackTable::Row track(vk_event_track_id_);
    track.scope = vk_event_scope_id_;
    TrackId track_id = context_->track_tracker->InternGpuTrack(track);
    tables::GpuSliceTable::Row row;
    row.ts = ts;
    row.dur = static_cast<int64_t>(event.duration_ns());
    row.track_id = track_id;
    row.name = vk_queue_submit_id_;
    if (event.has_vk_command_buffers()) {
      row.command_buffer = static_cast<int64_t>(*event.vk_command_buffers());
    }
    row.submission_id = event.submission_id();
    auto args_callback = [this, &event](ArgsTracker::BoundInserter* inserter) {
      inserter->AddArg(context_->storage->InternString("pid"),
                       Variadic::Integer(event.pid()));
      inserter->AddArg(context_->storage->InternString("tid"),
                       Variadic::Integer(event.tid()));
    };
    context_->slice_tracker->ScopedTyped(
        context_->storage->mutable_gpu_slice_table(), row, args_callback);
  }
}

void GpuEventParser::ParseGpuMemTotalEvent(int64_t ts, ConstBytes blob) {
  protos::pbzero::GpuMemTotalEvent::Decoder gpu_mem_total(blob.data, blob.size);

  TrackId track = kInvalidTrackId;
  const uint32_t pid = gpu_mem_total.pid();
  if (pid == 0) {
    // Pid 0 is used to indicate the global total
    track = context_->track_tracker->InternGlobalCounterTrack(
        TrackTracker::Group::kMemory, gpu_mem_total_name_id_, {},
        gpu_mem_total_unit_id_, gpu_mem_total_global_desc_id_);
  } else {
    // Process emitting the packet can be different from the pid in the event.
    UniqueTid utid = context_->process_tracker->UpdateThread(pid, pid);
    UniquePid upid = context_->storage->thread_table()[utid].upid().value_or(0);
    track = context_->track_tracker->InternProcessCounterTrack(
        gpu_mem_total_name_id_, upid, gpu_mem_total_unit_id_,
        gpu_mem_total_proc_desc_id_);
  }
  context_->event_tracker->PushCounter(
      ts, static_cast<double>(gpu_mem_total.size()), track);
}

}  // namespace perfetto::trace_processor
