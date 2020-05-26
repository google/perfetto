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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_MODULE_H_

#include "perfetto/base/build_config.h"
#include "src/trace_processor/importers/proto/graphics_event_parser.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/timestamped_trace_piece.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

class GraphicsEventModule
    : public ProtoImporterModuleBase<PERFETTO_BUILDFLAG(PERFETTO_TP_GRAPHICS)> {
 public:
  explicit GraphicsEventModule(TraceProcessorContext* context)
      : ProtoImporterModuleBase(context), parser_(context) {}

  ModuleResult ParsePacket(const protos::pbzero::TracePacket::Decoder& decoder,
                           const TimestampedTracePiece& ttp) {
    if (decoder.has_gpu_counter_event()) {
      parser_.ParseGpuCounterEvent(ttp.timestamp, decoder.gpu_counter_event());
      return ModuleResult::Handled();
    }

    if (decoder.has_gpu_render_stage_event()) {
      parser_.ParseGpuRenderStageEvent(ttp.timestamp,
                                       decoder.gpu_render_stage_event());
      return ModuleResult::Handled();
    }

    if (decoder.has_gpu_log()) {
      parser_.ParseGpuLog(ttp.timestamp, decoder.gpu_log());
      return ModuleResult::Handled();
    }

    if (decoder.has_graphics_frame_event()) {
      parser_.ParseGraphicsFrameEvent(ttp.timestamp,
                                      decoder.graphics_frame_event());
      return ModuleResult::Handled();
    }

    if (decoder.has_vulkan_memory_event()) {
      parser_.ParseVulkanMemoryEvent(ttp.packet_sequence_state,
                                     ttp.packet_sequence_state_generation,
                                     decoder.vulkan_memory_event());
      return ModuleResult::Handled();
    }

    return ModuleResult::Ignored();
  }

 private:
  GraphicsEventParser parser_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_GRAPHICS_EVENT_MODULE_H_
