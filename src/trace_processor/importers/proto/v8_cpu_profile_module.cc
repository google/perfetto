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
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/process_tracker.h"
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

// Field numbers for V8StreamingProfileExtensions (extending
// StreamingProfilePacket).
constexpr uint32_t kV8SampleKind = 1000;
constexpr uint32_t kV8SampleLeafLine = 1001;
constexpr uint32_t kV8SampleLeafColumn = 1002;
constexpr uint32_t kV8SampleSessionId = 1003;

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

const char* SampleKindToString(int32_t v) {
  switch (v) {
    case 1:
      return "NORMAL";
    case 2:
      return "PROGRAM";
    case 3:
      return "GC";
    case 4:
      return "IDLE";
    case 5:
      return "OTHER";
    default:
      return "UNKNOWN";
  }
}

void ReadRepeatedUint32(protozero::Field f, std::vector<uint32_t>* out) {
  if (f.type() == protozero::proto_utils::ProtoWireType::kLengthDelimited) {
    bool parse_error = false;
    protozero::PackedRepeatedFieldIterator<
        protozero::proto_utils::ProtoWireType::kVarInt, uint32_t>
        it(f.data(), f.size(), &parse_error);
    for (; it; ++it) {
      out->push_back(*it);
    }
  } else {
    out->push_back(f.as_uint32());
  }
}

void ReadRepeatedUint64(protozero::Field f, std::vector<uint64_t>* out) {
  if (f.type() == protozero::proto_utils::ProtoWireType::kLengthDelimited) {
    bool parse_error = false;
    protozero::PackedRepeatedFieldIterator<
        protozero::proto_utils::ProtoWireType::kVarInt, uint64_t>
        it(f.data(), f.size(), &parse_error);
    for (; it; ++it) {
      out->push_back(*it);
    }
  } else {
    out->push_back(f.as_uint64());
  }
}

void ReadRepeatedInt32(protozero::Field f, std::vector<int32_t>* out) {
  if (f.type() == protozero::proto_utils::ProtoWireType::kLengthDelimited) {
    bool parse_error = false;
    protozero::PackedRepeatedFieldIterator<
        protozero::proto_utils::ProtoWireType::kVarInt, int32_t>
        it(f.data(), f.size(), &parse_error);
    for (; it; ++it) {
      out->push_back(*it);
    }
  } else {
    out->push_back(f.as_int32());
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
                       args.ts, args.data);
}

void V8CpuProfileModule::ParseTracePacketData(protozero::ConstBytes bytes,
                                              int64_t ts,
                                              const TracePacketData& data) {
  V8CpuProfileSession::Decoder session(bytes);

  uint64_t session_id = session.session_id();
  V8CpuProfileSession::Phase phase =
      static_cast<V8CpuProfileSession::Phase>(session.phase());

  // Resolve utid from the sequence state's ThreadDescriptor.
  auto* gen = data.sequence_state.get();
  if (!gen)
    return;
  const auto& thread = gen->thread_descriptor();
  if (!thread.valid())
    return;

  uint32_t pid = static_cast<uint32_t>(thread.pid());
  uint32_t tid = static_cast<uint32_t>(thread.tid());
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, pid);

  StringPool* pool = context_->storage->mutable_string_pool();
  std::optional<StringId> source;
  if (session.has_source()) {
    source = pool->InternString(session.source());
  }

  if (phase == V8CpuProfileSession::PHASE_START) {
    tables::V8CpuProfileSessionTable::Row row;
    row.session_id = static_cast<int64_t>(session_id);
    row.utid = utid;
    row.source = source;
    row.start_ts = ts;
    if (session.has_wall_time_us())
      row.start_time_us = session.wall_time_us();
    if (session.has_thread_time_us())
      row.start_thread_ts = session.thread_time_us() * 1000;

    auto id = context_->storage->mutable_v8_cpu_profile_session_table()
                  ->Insert(row)
                  .id;
    open_sessions_.Insert({utid, static_cast<int64_t>(session_id)}, id);
  } else if (phase == V8CpuProfileSession::PHASE_END) {
    auto* id = open_sessions_.Find({utid, static_cast<int64_t>(session_id)});
    if (!id)
      return;
    auto rr = (*context_->storage->mutable_v8_cpu_profile_session_table())[*id];
    rr.set_end_ts(ts);
    if (session.has_wall_time_us())
      rr.set_end_time_us(session.wall_time_us());
    if (session.has_thread_time_us())
      rr.set_end_thread_ts(session.thread_time_us() * 1000);
    open_sessions_.Erase({utid, static_cast<int64_t>(session_id)});
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

V8CpuProfileModule::V8SampleExtensions
V8CpuProfileModule::ParseStreamingProfileExtensions(const uint8_t* packet_bytes,
                                                    size_t packet_size) {
  V8SampleExtensions ext;
  if (!packet_bytes || packet_size == 0)
    return ext;

  protozero::ProtoDecoder dec(packet_bytes, packet_size);
  for (auto f = dec.ReadField(); f.valid(); f = dec.ReadField()) {
    switch (f.id()) {
      case kV8SampleKind:
        ReadRepeatedInt32(f, &ext.sample_kind);
        break;
      case kV8SampleLeafLine:
        ReadRepeatedUint32(f, &ext.leaf_line);
        break;
      case kV8SampleLeafColumn:
        ReadRepeatedUint32(f, &ext.leaf_column);
        break;
      case kV8SampleSessionId:
        ReadRepeatedUint64(f, &ext.session_id);
        break;
      default:
        break;
    }
  }
  return ext;
}

void V8CpuProfileModule::OnSampleInserted(
    TraceProcessorContext* context,
    tables::CpuProfileStackSampleTable::Id sample_id,
    const V8SampleExtensions& exts,
    size_t index) {
  bool any = false;
  std::optional<int32_t> kind;
  std::optional<uint32_t> leaf_line;
  std::optional<uint32_t> leaf_column;
  std::optional<uint64_t> session_id;

  if (index < exts.sample_kind.size()) {
    kind = exts.sample_kind[index];
    any = true;
  }
  if (index < exts.leaf_line.size()) {
    leaf_line = exts.leaf_line[index];
    any = true;
  }
  if (index < exts.leaf_column.size()) {
    leaf_column = exts.leaf_column[index];
    any = true;
  }
  if (index < exts.session_id.size()) {
    session_id = exts.session_id[index];
    any = true;
  }
  if (!any)
    return;

  StringPool* pool = context->storage->mutable_string_pool();
  tables::V8CpuProfileSampleTable::Row row;
  row.cpu_profile_stack_sample_id = sample_id;
  if (kind)
    row.sample_kind = pool->InternString(SampleKindToString(*kind));
  if (leaf_line)
    row.leaf_line = *leaf_line;
  if (leaf_column)
    row.leaf_column = *leaf_column;
  if (session_id)
    row.session_id = static_cast<int64_t>(*session_id);
  context->storage->mutable_v8_cpu_profile_sample_table()->Insert(row);
}

}  // namespace perfetto::trace_processor
