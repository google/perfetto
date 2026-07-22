/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/v8_cpu_profile_module.h"

#include <cstdint>
#include <optional>
#include <utility>
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/v8_cpu_profile_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/track_event_thread_descriptor.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/v8/v8_cpu_profile_session.pbzero.h"

namespace perfetto::trace_processor {

namespace {

using TracePacket = protos::pbzero::TracePacket;
using V8CpuProfileSession = protos::pbzero::V8CpuProfileSession;

// Field numbers for V8FrameExtensions (extending Frame).
constexpr uint32_t kV8FrameTier = 1000;
constexpr uint32_t kV8FrameIsInlined = 1001;
constexpr uint32_t kV8FrameColumn = 1002;
constexpr uint32_t kV8FrameDeoptReasonIid = 1003;
constexpr uint32_t kV8FrameScriptId = 1004;

const char* TierToString(int32_t v) {
  switch (v) {
    case 1:
      return "IGNITION";
    case 2:
      return "SPARKPLUG";
    case 3:
      return "MAGLEV";
    case 4:
      return "TURBOFAN";
    case 5:
      return "BUILTIN";
    case 6:
      return "REGEXP";
    case 7:
      return "WASM";
    case 8:
      return "OTHER";
    default:
      return "UNKNOWN";
  }
}

}  // namespace

V8CpuProfileModule::V8CpuProfileModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(TracePacket::kV8CpuProfileSessionFieldNumber);
}

V8CpuProfileModule::~V8CpuProfileModule() = default;

ModuleResult V8CpuProfileModule::TokenizePacket(
    const TokenizePacketArgs& /*args*/) {
  return ModuleResult::Ignored();
}

void V8CpuProfileModule::ParseField(const ParseFieldArgs& args) {
  if (args.field.id() != TracePacket::kV8CpuProfileSessionFieldNumber)
    return;
  ParseTracePacketData(args.field.Cast<TracePacket::kV8CpuProfileSession>(),
                       args.ts, args.decoder.trusted_packet_sequence_id());
}

void V8CpuProfileModule::ParseTracePacketData(protozero::ConstBytes bytes,
                                              int64_t ts,
                                              uint32_t sequence_id) {
  V8CpuProfileSession::Decoder session(bytes);

  V8CpuProfileSession::Phase phase =
      static_cast<V8CpuProfileSession::Phase>(session.phase());

  StringPool* pool = context_->storage->mutable_string_pool();
  std::optional<StringId> source;
  if (session.has_source()) {
    source = pool->InternString(session.source());
  }

  if (phase == V8CpuProfileSession::PHASE_START) {
    context_->v8_cpu_profile_tracker->OnSessionStart(
        sequence_id, ts, source,
        session.has_wall_time_us()
            ? std::optional<int64_t>(session.wall_time_us())
            : std::nullopt,
        session.has_thread_time_us()
            ? std::optional<int64_t>(session.thread_time_us())
            : std::nullopt,
        session.has_pid() ? std::optional<int32_t>(session.pid())
                          : std::nullopt,
        session.has_tid() ? std::optional<int32_t>(session.tid())
                          : std::nullopt);
  } else if (phase == V8CpuProfileSession::PHASE_END) {
    context_->v8_cpu_profile_tracker->OnSessionEnd(
        sequence_id, ts,
        session.has_wall_time_us()
            ? std::optional<int64_t>(session.wall_time_us())
            : std::nullopt,
        session.has_thread_time_us()
            ? std::optional<int64_t>(session.thread_time_us())
            : std::nullopt);
  }
}

void V8CpuProfileModule::OnFrameInterned(TraceProcessorContext* context,
                                         PacketSequenceStateGeneration* state,
                                         FrameId frame_id,
                                         const uint8_t* frame_bytes,
                                         size_t frame_size) {
  if (!frame_bytes || frame_size == 0)
    return;

  bool any = false;
  std::optional<int32_t> tier;
  std::optional<bool> is_inlined;
  std::optional<uint32_t> column;
  std::optional<uint64_t> deopt_reason_iid;
  std::optional<int32_t> script_id;

  protozero::ProtoDecoder dec(frame_bytes, frame_size);
  for (auto f = dec.ReadField(); f.valid(); f = dec.ReadField()) {
    switch (f.id()) {
      case kV8FrameTier:
        tier = f.as_int32();
        any = true;
        break;
      case kV8FrameIsInlined:
        is_inlined = f.as_bool();
        any = true;
        break;
      case kV8FrameColumn:
        column = f.as_uint32();
        any = true;
        break;
      case kV8FrameDeoptReasonIid:
        deopt_reason_iid = f.as_uint64();
        any = true;
        break;
      case kV8FrameScriptId:
        script_id = f.as_int32();
        any = true;
        break;
      default:
        break;
    }
  }
  if (!any)
    return;

  StringPool* pool = context->storage->mutable_string_pool();
  tables::V8StackProfileFrameTable::Row row;
  row.frame_id = frame_id;
  if (tier)
    row.tier = pool->InternString(TierToString(*tier));
  if (is_inlined)
    row.is_inlined = *is_inlined ? 1u : 0u;
  if (column)
    row.column_number = *column;
  if (deopt_reason_iid) {
    auto deopt_reason = state->InternedStringView(
        protos::pbzero::InternedData::kFunctionNamesFieldNumber,
        *deopt_reason_iid);
    if (deopt_reason)
      row.deopt_reason = pool->InternString(*deopt_reason);
  }
  if (script_id)
    row.script_id = *script_id;
  context->storage->mutable_v8_stack_profile_frame_table()->Insert(row);
}

}  // namespace perfetto::trace_processor
