/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/v8_module.h"

#include <optional>

#include "protos/perfetto/trace/chrome/v8.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/v8_sequence_state.h"
#include "src/trace_processor/importers/proto/v8_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/v8_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::perfetto::protos::pbzero::TracePacket;
using ::perfetto::protos::pbzero::V8CodeMove;
using ::perfetto::protos::pbzero::V8InternalCode;
using ::perfetto::protos::pbzero::V8JsCode;
using ::perfetto::protos::pbzero::V8RegExpCode;
using ::perfetto::protos::pbzero::V8WasmCode;

}  // namespace

V8Module::V8Module(TraceProcessorContext* context)
    : context_(context), v8_tracker_(V8Tracker::GetOrCreate(context_)) {
  RegisterForField(TracePacket::kV8JsCodeFieldNumber, context_);
  RegisterForField(TracePacket::kV8InternalCodeFieldNumber, context_);
  RegisterForField(TracePacket::kV8WasmCodeFieldNumber, context_);
  RegisterForField(TracePacket::kV8RegExpCodeFieldNumber, context_);
  RegisterForField(TracePacket::kV8CodeMoveFieldNumber, context_);
}

V8Module::~V8Module() = default;

ModuleResult V8Module::TokenizePacket(const TracePacket::Decoder&,
                                      TraceBlobView* /*packet*/,
                                      int64_t /*packet_timestamp*/,
                                      PacketSequenceState* /*state*/,
                                      uint32_t /*field_id*/) {
  return ModuleResult::Ignored();
}

void V8Module::ParseTracePacketData(const TracePacket::Decoder& decoder,
                                    int64_t ts,
                                    const TracePacketData& data,
                                    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kV8JsCodeFieldNumber:
      ParseV8JsCode(decoder.v8_js_code(), ts, data);
      break;
    case TracePacket::kV8InternalCodeFieldNumber:
      ParseV8InternalCode(decoder.v8_internal_code(), ts, data);
      break;
    case TracePacket::kV8WasmCodeFieldNumber:
      ParseV8WasmCode(decoder.v8_wasm_code(), ts, data);
      break;
    case TracePacket::kV8RegExpCodeFieldNumber:
      ParseV8RegExpCode(decoder.v8_reg_exp_code(), ts, data);
      break;
    case TracePacket::kV8CodeMoveFieldNumber:
      ParseV8CodeMove(decoder.v8_code_move(), ts, data);
      break;
    default:
      break;
  }
}

void V8Module::ParseV8JsCode(protozero::ConstBytes bytes,
                             int64_t ts,
                             const TracePacketData& data) {
  V8SequenceState& state =
      *V8SequenceState::GetOrCreate(data.sequence_state->state());

  V8JsCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  auto v8_function_id =
      state.GetOrInsertJsFunction(code.v8_js_function_iid(), *v8_isolate_id);
  if (!v8_function_id) {
    return;
  }

  v8_tracker_->AddJsCode(ts, *v8_isolate_id, *v8_function_id, code);
}

void V8Module::ParseV8InternalCode(protozero::ConstBytes bytes,
                                   int64_t ts,
                                   const TracePacketData& data) {
  V8SequenceState& state =
      *V8SequenceState::GetOrCreate(data.sequence_state->state());

  V8InternalCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  v8_tracker_->AddInternalCode(ts, *v8_isolate_id, code);
}

void V8Module::ParseV8WasmCode(protozero::ConstBytes bytes,
                               int64_t ts,
                               const TracePacketData& data) {
  V8SequenceState& state =
      *V8SequenceState::GetOrCreate(data.sequence_state->state());

  V8WasmCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  auto v8_wasm_script_id =
      state.GetOrInsertWasmScript(code.v8_wasm_script_iid(), *v8_isolate_id);
  if (!v8_wasm_script_id) {
    return;
  }

  v8_tracker_->AddWasmCode(ts, *v8_isolate_id, *v8_wasm_script_id, code);
}

void V8Module::ParseV8RegExpCode(protozero::ConstBytes bytes,
                                 int64_t ts,
                                 const TracePacketData& data) {
  V8SequenceState& state =
      *V8SequenceState::GetOrCreate(data.sequence_state->state());

  V8RegExpCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  v8_tracker_->AddRegExpCode(ts, *v8_isolate_id, code);
}

void V8Module::ParseV8CodeMove(protozero::ConstBytes bytes,
                               int64_t,
                               const TracePacketData& data) {
  V8SequenceState& state =
      *V8SequenceState::GetOrCreate(data.sequence_state->state());
  protos::pbzero::V8CodeMove::Decoder v8_code_move(bytes);

  std::optional<tables::V8IsolateTable::Id> isolate_id =
      state.GetOrInsertIsolate(v8_code_move.isolate_iid());
  if (!isolate_id) {
    return;
  }

  // TODO(carlscab): Implement
}

}  // namespace trace_processor
}  // namespace perfetto
