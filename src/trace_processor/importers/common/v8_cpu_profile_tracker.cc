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

#include "src/trace_processor/importers/common/v8_cpu_profile_tracker.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

V8CpuProfileTracker::V8CpuProfileTracker(TraceProcessorContext* context)
    : context_(context) {}

V8CpuProfileTracker::~V8CpuProfileTracker() = default;

void V8CpuProfileTracker::Parse(int64_t ts, LegacyV8CpuProfileEvent event) {
  base::Status status =
      AddSample(ts, event.session_id, event.pid, event.tid, event.callsite_id);
  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::legacy_v8_cpu_profile_invalid_sample);
  }
}

void V8CpuProfileTracker::SetStartTsForSessionAndPid(uint64_t session_id,
                                                     uint32_t pid,
                                                     int64_t ts) {
  auto [it, inserted] = state_by_session_and_pid_.Insert(
      std::make_pair(session_id, pid),
      State{ts, base::FlatHashMap<uint32_t, CallsiteId>(),
            base::FlatHashMap<uint32_t, uint32_t>(), nullptr, std::nullopt,
            std::nullopt, std::nullopt});
  it->ts = ts;
  if (inserted) {
    it->mapping = &context_->mapping_tracker->CreateDummyMapping("");
  }
}

base::Status V8CpuProfileTracker::AddCallsite(
    uint64_t session_id,
    uint32_t pid,
    uint32_t raw_callsite_id,
    std::optional<uint32_t> parent_raw_callsite_id,
    base::StringView script_url,
    base::StringView function_name,
    const std::vector<uint32_t>& raw_children_callsite_ids) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state) {
    return base::ErrStatus(
        "v8 profile id does not exist: cannot insert callsite");
  }

  auto* existing_callsite = state->callsites.Find(raw_callsite_id);
  if (existing_callsite) {
    return base::ErrStatus("v8 profile: callsite with id already exists");
  }

  FrameId frame_id =
      state->mapping->InternDummyFrame(function_name, script_url);

  // V8 and NodeJS/DevTools have different formats they expect for parent <->
  // child releationships for stack sampling data.
  //
  // V8 works by providing the parent for every frame, while NodeJS/Devtools
  // follow the devtools protocol [1] which specifies the children. Try to
  // work with either.
  //
  // [1]
  // https://chromedevtools.github.io/devtools-protocol/tot/Profiler/#type-ProfileNode
  if (!parent_raw_callsite_id) {
    auto* parent_ptr = state->callsite_inferred_parents.Find(raw_callsite_id);
    if (parent_ptr) {
      parent_raw_callsite_id = *parent_ptr;
    }
  }

  CallsiteId callsite_id;
  uint32_t depth;
  if (parent_raw_callsite_id) {
    auto* parent_id = state->callsites.Find(*parent_raw_callsite_id);
    if (!parent_id) {
      return base::ErrStatus(
          "v8 profile parent id does not exist: cannot insert callsite");
    }
    auto row =
        context_->storage->stack_profile_callsite_table().FindById(*parent_id);
    callsite_id = context_->stack_profile_tracker->InternCallsite(
        *parent_id, frame_id, row->depth() + 1);
    depth = row->depth() + 1;
  } else {
    callsite_id = context_->stack_profile_tracker->InternCallsite(std::nullopt,
                                                                  frame_id, 0);
    depth = 0;
  }

  // We already asserted above that we don't already have a node with this
  // callsite id.
  PERFETTO_CHECK(state->callsites.Insert(raw_callsite_id, callsite_id).second);

  // Insert the children so it can be picked up if the node is added in the
  // future. Also go through all the nodes in the table itself and fix the
  // parent/depth relationships up if the node is already in the table.
  for (uint32_t raw_child_id : raw_children_callsite_ids) {
    auto [it, inserted] =
        state->callsite_inferred_parents.Insert(raw_child_id, raw_callsite_id);
    if (!inserted) {
      return base::ErrStatus(
          "v8 profile: multiple nodes specify the same node id %u as child",
          raw_child_id);
    }

    auto* child_callsite_id = state->callsites.Find(raw_child_id);

    // This means that we havent' seen the node yet. We expect it to appear in
    // the future and be picked up by the `!parent_raw_callsite_id` above when
    // it does.
    if (!child_callsite_id) {
      continue;
    }
    auto row =
        context_->storage->mutable_stack_profile_callsite_table()->FindById(
            *child_callsite_id);
    PERFETTO_CHECK(row);
    row->set_depth(depth + 1);
    row->set_parent_id(callsite_id);
  }
  return base::OkStatus();
}

base::StatusOr<int64_t> V8CpuProfileTracker::AddDeltaAndGetTs(
    uint64_t session_id,
    uint32_t pid,
    int64_t delta_ts) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state) {
    return base::ErrStatus(
        "v8 profile id does not exist: cannot compute timestamp from delta");
  }
  state->ts += delta_ts;
  return state->ts;
}

base::Status V8CpuProfileTracker::AddSample(int64_t ts,
                                            uint64_t session_id,
                                            uint32_t pid,
                                            uint32_t tid,
                                            uint32_t raw_callsite_id) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state) {
    return base::ErrStatus("v8 callsite id does not exist: cannot add sample");
  }
  auto* id = state->callsites.Find(raw_callsite_id);
  if (!id) {
    return base::ErrStatus("v8 callsite id does not exist: cannot add sample");
  }
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, pid);
  auto* samples = context_->storage->mutable_cpu_profile_stack_sample_table();
  samples->Insert({ts, *id, utid, 0});
  return base::OkStatus();
}

void V8CpuProfileTracker::BeginSession(uint64_t session_id,
                                       uint32_t pid,
                                       uint32_t tid,
                                       int64_t start_ts,
                                       std::optional<int64_t> start_time_us,
                                       std::optional<int64_t> start_thread_ts,
                                       std::optional<base::StringView> source) {
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, pid);

  tables::V8CpuProfileSessionTable::Row row;
  row.session_id = static_cast<int64_t>(session_id);
  row.utid = utid;
  row.start_ts = start_ts;
  if (start_time_us) {
    row.start_time_us = *start_time_us;
  }
  if (start_thread_ts) {
    row.start_thread_ts = *start_thread_ts;
  }
  if (source) {
    row.source = context_->storage->InternString(*source);
  }
  auto session_row_id =
      context_->storage->mutable_v8_cpu_profile_session_table()->Insert(row).id;

  auto [it, inserted] = state_by_session_and_pid_.Insert(
      std::make_pair(session_id, pid),
      State{start_ts, base::FlatHashMap<uint32_t, CallsiteId>(),
            base::FlatHashMap<uint32_t, uint32_t>(), nullptr, session_row_id,
            std::nullopt, std::nullopt});
  it->ts = start_ts;
  it->session_row_id = session_row_id;
  it->current_chunk_row_id = std::nullopt;
  it->pending_chunk = std::nullopt;
  if (inserted || !it->mapping) {
    it->mapping = &context_->mapping_tracker->CreateDummyMapping("");
  }
}

void V8CpuProfileTracker::EndSession(uint64_t session_id,
                                     uint32_t pid,
                                     int64_t end_ts,
                                     std::optional<int64_t> end_time_us,
                                     std::optional<int64_t> end_thread_ts) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state || !state->session_row_id) {
    return;
  }
  auto rr = context_->storage->mutable_v8_cpu_profile_session_table()->FindById(
      *state->session_row_id);
  if (!rr) {
    return;
  }
  rr->set_end_ts(end_ts);
  if (end_time_us) {
    rr->set_end_time_us(*end_time_us);
  }
  if (end_thread_ts) {
    rr->set_end_thread_ts(*end_thread_ts);
  }
}

void V8CpuProfileTracker::BeginChunk(uint64_t session_id,
                                     uint32_t pid,
                                     int64_t ts,
                                     std::optional<int64_t> thread_ts) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state || !state->session_row_id) {
    return;
  }
  // Defer the chunk row insert until we actually see content for this chunk.
  // Chunks with no nodes, samples, or trace_id_mappings are dropped.
  state->current_chunk_row_id = std::nullopt;
  state->pending_chunk = State::PendingChunk{ts, thread_ts};
}

bool V8CpuProfileTracker::EnsureChunkRow(State* state) {
  if (state->current_chunk_row_id) {
    return true;
  }
  if (!state->session_row_id || !state->pending_chunk) {
    return false;
  }
  tables::V8CpuProfileChunkTable::Row row;
  row.v8_cpu_profile_session_id = *state->session_row_id;
  row.ts = state->pending_chunk->ts;
  if (state->pending_chunk->thread_ts) {
    row.thread_ts = *state->pending_chunk->thread_ts;
  }
  state->current_chunk_row_id =
      context_->storage->mutable_v8_cpu_profile_chunk_table()->Insert(row).id;
  state->pending_chunk = std::nullopt;
  return true;
}

void V8CpuProfileTracker::AddNodeMetadata(
    uint64_t session_id,
    uint32_t pid,
    uint32_t raw_callsite_id,
    std::optional<base::StringView> function_name,
    std::optional<base::StringView> url,
    std::optional<int32_t> script_id,
    std::optional<int32_t> line,
    std::optional<int32_t> column,
    std::optional<base::StringView> code_type,
    std::optional<base::StringView> deopt_reason) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state || !state->session_row_id) {
    return;
  }
  if (!EnsureChunkRow(state)) {
    return;
  }
  auto* callsite_id = state->callsites.Find(raw_callsite_id);
  if (!callsite_id) {
    return;
  }

  tables::V8CpuProfileNodeTable::Row row;
  row.v8_cpu_profile_session_id = *state->session_row_id;
  row.v8_cpu_profile_chunk_id = *state->current_chunk_row_id;
  row.node_id = raw_callsite_id;
  row.callsite_id = *callsite_id;
  if (function_name) {
    row.function_name = context_->storage->InternString(*function_name);
  }
  if (url) {
    row.url = context_->storage->InternString(*url);
  }
  if (script_id) {
    row.script_id = *script_id;
  }
  if (line) {
    row.line = *line;
  }
  if (column) {
    row.column = *column;
  }
  if (code_type) {
    row.code_type = context_->storage->InternString(*code_type);
  }
  if (deopt_reason) {
    row.deopt_reason = context_->storage->InternString(*deopt_reason);
  }
  context_->storage->mutable_v8_cpu_profile_node_table()->Insert(row);
}

base::Status V8CpuProfileTracker::AddSampleWithMeta(
    int64_t ts,
    uint64_t session_id,
    uint32_t pid,
    uint32_t tid,
    uint32_t raw_callsite_id,
    std::optional<int32_t> line,
    std::optional<int32_t> column) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state) {
    return base::ErrStatus("v8 callsite id does not exist: cannot add sample");
  }
  auto* id = state->callsites.Find(raw_callsite_id);
  if (!id) {
    return base::ErrStatus("v8 callsite id does not exist: cannot add sample");
  }
  UniqueTid utid = context_->process_tracker->UpdateThread(tid, pid);
  auto* samples = context_->storage->mutable_cpu_profile_stack_sample_table();
  auto sample_id = samples->Insert({ts, *id, utid, 0}).id;

  if (state->session_row_id && EnsureChunkRow(state)) {
    tables::V8CpuProfileSampleTable::Row meta;
    meta.cpu_profile_stack_sample_id = sample_id;
    meta.v8_cpu_profile_session_id = *state->session_row_id;
    meta.v8_cpu_profile_chunk_id = *state->current_chunk_row_id;
    meta.node_id = raw_callsite_id;
    if (line) {
      meta.line = *line;
    }
    if (column) {
      meta.column = *column;
    }
    context_->storage->mutable_v8_cpu_profile_sample_table()->Insert(meta);
  }
  return base::OkStatus();
}

void V8CpuProfileTracker::AddTraceIdMapping(uint64_t session_id,
                                            uint32_t pid,
                                            uint64_t trace_id,
                                            uint32_t node_id) {
  auto* state = state_by_session_and_pid_.Find(std::make_pair(session_id, pid));
  if (!state || !state->session_row_id) {
    return;
  }
  if (!EnsureChunkRow(state)) {
    return;
  }
  tables::V8CpuProfileTraceIdTable::Row row;
  row.v8_cpu_profile_session_id = *state->session_row_id;
  row.v8_cpu_profile_chunk_id = *state->current_chunk_row_id;
  row.trace_id = static_cast<int64_t>(trace_id);
  row.node_id = node_id;
  context_->storage->mutable_v8_cpu_profile_trace_id_table()->Insert(row);
}

}  // namespace perfetto::trace_processor
