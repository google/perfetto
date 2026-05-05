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

#include <cstdint>
#include <optional>

#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/chrome/v8.pbzero.h"
#include "protos/perfetto/trace/chrome/v8_cpu_profile.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/v8_cpu_profile_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/v8_sequence_state.h"
#include "src/trace_processor/importers/proto/v8_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/v8_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::perfetto::protos::pbzero::TracePacket;
using ::perfetto::protos::pbzero::V8CodeDefaults;
using ::perfetto::protos::pbzero::V8CodeMove;
using ::perfetto::protos::pbzero::V8CpuProfileChunk;
using ::perfetto::protos::pbzero::V8InternalCode;
using ::perfetto::protos::pbzero::V8JsCode;
using ::perfetto::protos::pbzero::V8RegExpCode;
using ::perfetto::protos::pbzero::V8WasmCode;

}  // namespace

V8Module::V8Module(ProtoImporterModuleContext* module_context,
                   TraceProcessorContext* context)
    : ProtoImporterModule(module_context),
      context_(context),
      v8_tracker_(std::make_unique<V8Tracker>(context)),
      cpu_profile_tracker_(std::make_unique<V8CpuProfileTracker>(context)) {
  RegisterForField(TracePacket::kV8JsCodeFieldNumber);
  RegisterForField(TracePacket::kV8InternalCodeFieldNumber);
  RegisterForField(TracePacket::kV8WasmCodeFieldNumber);
  RegisterForField(TracePacket::kV8RegExpCodeFieldNumber);
  RegisterForField(TracePacket::kV8CodeMoveFieldNumber);
  RegisterForField(TracePacket::kV8CpuProfileChunkFieldNumber);
}

V8Module::~V8Module() = default;

ModuleResult V8Module::TokenizePacket(
    const TracePacket::Decoder&,
    TraceBlobView* /*packet*/,
    int64_t /*packet_timestamp*/,
    RefPtr<PacketSequenceStateGeneration> /*state*/,
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
    case TracePacket::kV8CpuProfileChunkFieldNumber:
      ParseV8CpuProfileChunk(decoder.v8_cpu_profile_chunk(), ts, data);
      break;
    default:
      break;
  }
}

template <typename CodeDecoder>
std::optional<UniqueTid> V8Module::GetUtid(
    PacketSequenceStateGeneration& generation,
    IsolateId isolate_id,
    const CodeDecoder& code) {
  auto* pid = isolate_to_pid_.Find(isolate_id);
  if (!pid) {
    tables::ProcessTable::Id upid(
        context_->storage->v8_isolate_table().FindById(isolate_id)->upid());
    pid = isolate_to_pid_
              .Insert(
                  isolate_id,
                  static_cast<uint32_t>(
                      context_->storage->process_table().FindById(upid)->pid()))
              .first;
  }

  if (code.has_tid()) {
    return context_->process_tracker->UpdateThread(code.tid(), *pid);
  }

  if (auto tid = GetDefaultTid(generation); tid.has_value()) {
    return context_->process_tracker->UpdateThread(*tid, *pid);
  }

  return std::nullopt;
}

std::optional<uint32_t> V8Module::GetDefaultTid(
    PacketSequenceStateGeneration& generation) const {
  auto* tp_defaults = generation.GetTracePacketDefaults();
  if (!tp_defaults) {
    context_->storage->IncrementStats(stats::v8_no_defaults);
    return std::nullopt;
  }
  if (!tp_defaults->has_v8_code_defaults()) {
    context_->storage->IncrementStats(stats::v8_no_defaults);
    return std::nullopt;
  }

  V8CodeDefaults::Decoder v8_defaults(tp_defaults->v8_code_defaults());

  if (!v8_defaults.has_tid()) {
    context_->storage->IncrementStats(stats::v8_no_defaults);
    return std::nullopt;
  }

  return v8_defaults.tid();
}

void V8Module::ParseV8JsCode(protozero::ConstBytes bytes,
                             int64_t ts,
                             const TracePacketData& data) {
  V8SequenceState& state =
      *data.sequence_state->GetCustomState<V8SequenceState>(v8_tracker_.get());

  V8JsCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(data.sequence_state.get(),
                                                code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  std::optional<UniqueTid> utid =
      GetUtid(*data.sequence_state, *v8_isolate_id, code);
  if (!utid) {
    return;
  }

  auto v8_function_id = state.GetOrInsertJsFunction(
      data.sequence_state.get(), code.v8_js_function_iid(), *v8_isolate_id);
  if (!v8_function_id) {
    return;
  }

  v8_tracker_->AddJsCode(ts, *utid, *v8_isolate_id, *v8_function_id, code);
}

void V8Module::ParseV8InternalCode(protozero::ConstBytes bytes,
                                   int64_t ts,
                                   const TracePacketData& data) {
  V8SequenceState& state =
      *data.sequence_state->GetCustomState<V8SequenceState>(v8_tracker_.get());

  V8InternalCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(data.sequence_state.get(),
                                                code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  std::optional<UniqueTid> utid =
      GetUtid(*data.sequence_state, *v8_isolate_id, code);
  if (!utid) {
    return;
  }

  v8_tracker_->AddInternalCode(ts, *utid, *v8_isolate_id, code);
}

void V8Module::ParseV8WasmCode(protozero::ConstBytes bytes,
                               int64_t ts,
                               const TracePacketData& data) {
  V8SequenceState& state =
      *data.sequence_state->GetCustomState<V8SequenceState>(v8_tracker_.get());

  V8WasmCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(data.sequence_state.get(),
                                                code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  auto v8_wasm_script_id = state.GetOrInsertWasmScript(
      data.sequence_state.get(), code.v8_wasm_script_iid(), *v8_isolate_id);
  if (!v8_wasm_script_id) {
    return;
  }

  std::optional<UniqueTid> utid =
      GetUtid(*data.sequence_state, *v8_isolate_id, code);
  if (!utid) {
    return;
  }

  v8_tracker_->AddWasmCode(ts, *utid, *v8_isolate_id, *v8_wasm_script_id, code);
}

void V8Module::ParseV8RegExpCode(protozero::ConstBytes bytes,
                                 int64_t ts,
                                 const TracePacketData& data) {
  V8SequenceState& state =
      *data.sequence_state->GetCustomState<V8SequenceState>(v8_tracker_.get());

  V8RegExpCode::Decoder code(bytes);

  auto v8_isolate_id = state.GetOrInsertIsolate(data.sequence_state.get(),
                                                code.v8_isolate_iid());
  if (!v8_isolate_id) {
    return;
  }

  std::optional<UniqueTid> utid =
      GetUtid(*data.sequence_state, *v8_isolate_id, code);
  if (!utid) {
    return;
  }

  v8_tracker_->AddRegExpCode(ts, *utid, *v8_isolate_id, code);
}

void V8Module::ParseV8CodeMove(protozero::ConstBytes bytes,
                               int64_t ts,
                               const TracePacketData& data) {
  V8SequenceState& state =
      *data.sequence_state->GetCustomState<V8SequenceState>(v8_tracker_.get());
  protos::pbzero::V8CodeMove::Decoder v8_code_move(bytes);

  std::optional<IsolateId> isolate_id = state.GetOrInsertIsolate(
      data.sequence_state.get(), v8_code_move.isolate_iid());
  if (!isolate_id) {
    return;
  }

  std::optional<UniqueTid> utid =
      GetUtid(*data.sequence_state, *isolate_id, v8_code_move);
  if (!utid) {
    return;
  }

  v8_tracker_->MoveCode(ts, *utid, *isolate_id, v8_code_move);
}

void V8Module::ParseV8CpuProfileChunk(protozero::ConstBytes bytes,
                                      int64_t ts,
                                      const TracePacketData& /*data*/) {
  V8CpuProfileChunk::Decoder chunk(bytes);

  uint64_t session_id = chunk.session_id();
  uint32_t pid = chunk.pid();
  uint32_t tid = chunk.tid();

  auto source_to_str =
      [](V8CpuProfileChunk::ProfileSource s) -> base::StringView {
    switch (s) {
      case V8CpuProfileChunk::PROFILE_SOURCE_INSPECTOR:
        return "Inspector";
      case V8CpuProfileChunk::PROFILE_SOURCE_SELF_PROFILING:
        return "SelfProfiling";
      case V8CpuProfileChunk::PROFILE_SOURCE_INTERNAL:
        return "Internal";
      default:
        return "Unspecified";
    }
  };

  // Session start.
  if (chunk.has_session_start()) {
    V8CpuProfileChunk::SessionStart::Decoder start(chunk.session_start());

    int64_t start_ts = ts;
    std::optional<int64_t> start_time_us;
    if (start.has_start_time_us()) {
      start_time_us = start.start_time_us();
      std::optional<int64_t> trace_ts = context_->clock_tracker->ToTraceTime(
          ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_MONOTONIC),
          *start_time_us * 1000);
      if (trace_ts) {
        start_ts = *trace_ts;
      }
    }
    std::optional<int64_t> start_thread_ts;
    if (start.has_start_thread_time_us()) {
      start_thread_ts = start.start_thread_time_us() * 1000;
    }
    std::optional<base::StringView> source;
    if (start.has_source()) {
      source = source_to_str(
          static_cast<V8CpuProfileChunk::ProfileSource>(start.source()));
    }
    cpu_profile_tracker_->BeginSession(session_id, pid, tid, start_ts,
                                       start_time_us, start_thread_ts, source);
    return;
  }

  // Session end.
  if (chunk.has_session_end()) {
    V8CpuProfileChunk::SessionEnd::Decoder end_data(chunk.session_end());
    std::optional<int64_t> end_time_us;
    if (end_data.has_end_time_us()) {
      end_time_us = end_data.end_time_us();
    }
    std::optional<int64_t> end_thread_ts;
    if (end_data.has_end_thread_time_us()) {
      end_thread_ts = end_data.end_thread_time_us() * 1000;
    }
    cpu_profile_tracker_->EndSession(session_id, pid, ts, end_time_us,
                                     end_thread_ts);
    return;
  }

  // Data chunk: open a new chunk row that subsequent nodes/samples will be
  // attached to.
  std::optional<int64_t> chunk_thread_ts;
  if (chunk.has_thread_time_us()) {
    chunk_thread_ts = chunk.thread_time_us() * 1000;
  }
  cpu_profile_tracker_->BeginChunk(session_id, pid, ts, chunk_thread_ts);

  // Data chunk: nodes, samples and trace_id mappings.
  for (auto it = chunk.nodes(); it; ++it) {
    V8CpuProfileChunk::Node::Decoder node(*it);
    V8CpuProfileChunk::Node::CallFrame::Decoder cf(node.call_frame());

    base::StringView url =
        cf.has_url() ? base::StringView(cf.url()) : base::StringView();
    base::StringView function_name = cf.has_function_name()
                                         ? base::StringView(cf.function_name())
                                         : base::StringView();

    std::optional<uint32_t> parent;
    if (node.has_parent_id() && node.parent_id() != 0) {
      parent = node.parent_id();
    }

    base::Status status = cpu_profile_tracker_->AddCallsite(
        session_id, pid, node.id(), parent, url, function_name, {});
    if (!status.ok()) {
      context_->storage->IncrementStats(
          stats::legacy_v8_cpu_profile_invalid_callsite);
      continue;
    }

    std::optional<base::StringView> fn_name;
    std::optional<base::StringView> url_opt;
    std::optional<int32_t> script_id;
    std::optional<int32_t> line;
    std::optional<int32_t> column;
    std::optional<base::StringView> code_type;
    std::optional<base::StringView> deopt_reason;
    if (cf.has_function_name()) {
      fn_name = function_name;
    }
    if (cf.has_url()) {
      url_opt = url;
    }
    if (cf.has_script_id()) {
      script_id = cf.script_id();
    }
    if (cf.has_line_number()) {
      line = cf.line_number();
    }
    if (cf.has_column_number()) {
      column = cf.column_number();
    }
    if (cf.has_code_type()) {
      code_type = cf.code_type();
    }
    if (node.has_deopt_reason()) {
      deopt_reason = node.deopt_reason();
    }
    cpu_profile_tracker_->AddNodeMetadata(session_id, pid, node.id(), fn_name,
                                          url_opt, script_id, line, column,
                                          code_type, deopt_reason);
  }

  // Walk samples + per-sample line/column iterators in lockstep. Per the
  // proto, `time_deltas_us[0]` is relative to the prior anchor (session start
  // for the first chunk, or the previous chunk's last sample for subsequent
  // chunks). The tracker keeps that running anchor in `state->ts`, seeded by
  // BeginSession.
  bool samples_err = false;
  bool deltas_err = false;
  bool lines_err = false;
  bool cols_err = false;
  auto samples_it = chunk.samples(&samples_err);
  auto deltas_it = chunk.time_deltas_us(&deltas_err);
  auto lines_it = chunk.lines(&lines_err);
  auto cols_it = chunk.columns(&cols_err);
  while (samples_it && deltas_it) {
    base::StatusOr<int64_t> sample_ts = cpu_profile_tracker_->AddDeltaAndGetTs(
        session_id, pid, static_cast<int64_t>(*deltas_it) * 1000);
    if (!sample_ts.ok()) {
      context_->storage->IncrementStats(
          stats::legacy_v8_cpu_profile_invalid_sample);
      ++samples_it;
      ++deltas_it;
      if (lines_it) {
        ++lines_it;
      }
      if (cols_it) {
        ++cols_it;
      }
      continue;
    }
    std::optional<int32_t> line;
    std::optional<int32_t> column;
    if (lines_it) {
      line = *lines_it;
      ++lines_it;
    }
    if (cols_it) {
      column = *cols_it;
      ++cols_it;
    }
    base::Status s = cpu_profile_tracker_->AddSampleWithMeta(
        *sample_ts, session_id, pid, tid, *samples_it, line, column);
    if (!s.ok()) {
      context_->storage->IncrementStats(
          stats::legacy_v8_cpu_profile_invalid_sample);
    }
    ++samples_it;
    ++deltas_it;
  }

  for (auto it = chunk.trace_id_mappings(); it; ++it) {
    V8CpuProfileChunk::TraceIdMapping::Decoder mapping(*it);
    cpu_profile_tracker_->AddTraceIdMapping(session_id, pid, mapping.trace_id(),
                                            mapping.node_id());
  }
}

}  // namespace trace_processor
}  // namespace perfetto
