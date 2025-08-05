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

#include "src/trace_processor/importers/proto/proto_importer_module.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

ProtoImporterModule::ProtoImporterModule(
    ProtoImporterModuleContext* module_context)
    : module_context_(module_context) {}

ProtoImporterModule::~ProtoImporterModule() {}

ModuleResult ProtoImporterModule::TokenizePacket(
    const protos::pbzero::TracePacket_Decoder&,
    TraceBlobView* /*packet*/,
    int64_t /*packet_timestamp*/,
    RefPtr<PacketSequenceStateGeneration> /*sequence_state*/,
    uint32_t /*field_id*/) {
  return ModuleResult::Ignored();
}

void ProtoImporterModule::ParseTracePacketData(
    const protos::pbzero::TracePacket_Decoder&,
    int64_t /*ts*/,
    const TracePacketData&,
    uint32_t /*field_id*/) {}

void ProtoImporterModule::ParseTraceConfig(
    const protos::pbzero::TraceConfig_Decoder&) {}

void ProtoImporterModule::RegisterForField(uint32_t field_id) {
  if (module_context_->modules_by_field.size() <= field_id) {
    module_context_->modules_by_field.resize(field_id + 1);
  }
  module_context_->modules_by_field[field_id].push_back(this);
}

void ProtoImporterModuleContext::PushFtraceEvent(uint32_t cpu,
                                                 int64_t ts,
                                                 TracePacketData data) {
  auto& streams = ftrace_event_streams;
  if (PERFETTO_UNLIKELY(cpu >= streams.size())) {
    size_t old_size = streams.size();
    streams.resize(cpu + 1);
    for (size_t i = old_size; i <= cpu; ++i) {
      streams[i] = ftrace_stream_factory(static_cast<uint32_t>(i));
    }
  }
  streams[cpu]->Push(ts, std::move(data));
}

void ProtoImporterModuleContext::PushEtwEvent(uint32_t cpu,
                                              int64_t ts,
                                              TracePacketData data) {
  auto& streams = etw_event_streams;
  if (PERFETTO_UNLIKELY(cpu >= streams.size())) {
    size_t old_size = streams.size();
    streams.resize(cpu + 1);
    for (size_t i = old_size; i <= cpu; ++i) {
      streams[i] = etw_stream_factory(static_cast<uint32_t>(i));
    }
  }
  streams[cpu]->Push(ts, std::move(data));
}

void ProtoImporterModuleContext::PushInlineSchedSwitch(uint32_t cpu,
                                                       int64_t ts,
                                                       InlineSchedSwitch data) {
  auto& streams = inline_sched_switch_streams;
  if (PERFETTO_UNLIKELY(cpu >= streams.size())) {
    size_t old_size = streams.size();
    streams.resize(cpu + 1);
    for (size_t i = old_size; i <= cpu; ++i) {
      streams[i] = inline_sched_switch_stream_factory(static_cast<uint32_t>(i));
    }
  }
  streams[cpu]->Push(ts, data);
}

void ProtoImporterModuleContext::PushInlineSchedWaking(uint32_t cpu,
                                                       int64_t ts,
                                                       InlineSchedWaking data) {
  auto& streams = inline_sched_waking_streams;
  if (PERFETTO_UNLIKELY(cpu >= streams.size())) {
    size_t old_size = streams.size();
    streams.resize(cpu + 1);
    for (size_t i = old_size; i <= cpu; ++i) {
      streams[i] = inline_sched_waking_stream_factory(static_cast<uint32_t>(i));
    }
  }
  streams[cpu]->Push(ts, data);
}

}  // namespace perfetto::trace_processor
