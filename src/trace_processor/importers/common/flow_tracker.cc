/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <atomic>
#include <limits>
#include <optional>

#include <stdint.h>

#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/flow_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

FlowTracker::FlowTracker(TraceProcessorContext* context) : context_(context) {
  name_key_id_ = context_->storage->InternString("name");
  cat_key_id_ = context_->storage->InternString("cat");
}

FlowTracker::~FlowTracker() = default;

namespace {
std::atomic<uint64_t> g_slice_table_ts_reads{0};
}  // namespace

uint64_t FlowTracker::slice_table_ts_reads() {
  return g_slice_table_ts_reads.load(std::memory_order_relaxed);
}

FlowTracker::FlowSource FlowTracker::Source(SliceId id) const {
  return {id, context_->slice_tracker->StartOfRecentSlice(id)};
}

std::optional<int64_t> FlowTracker::SourceTs(const FlowSource& source) {
  if (source.ts)
    return source.ts;
  if (auto ts = context_->slice_tracker->StartOfRecentSlice(source.id))
    return ts;
  g_slice_table_ts_reads.fetch_add(1, std::memory_order_relaxed);
  if (!context_->slice_tracker->WritesTable())
    return std::nullopt;
  return context_->slice_tracker->StartTsOf(source.id);
}

/* TODO: if we report a flow event earlier that a corresponding slice then
  flow event would not be added, and it will increase "flow_no_enclosing_slice"
  In catapult, it was possible to report a flow after an enclosing slice if
  timestamps were equal. But because of our sequential processing of a trace
  it is a bit tricky to make it here.
  We suspect that this case is too rare or impossible */
void FlowTracker::Begin(TrackId track_id, FlowId flow_id) {
  auto open = context_->slice_tracker->GetTopmostOpenSlice(track_id);
  if (!open) {
    context_->stats_tracker->IncrementStats(stats::flow_no_enclosing_slice);
    return;
  }
  BeginAt({open->id, open->ts}, flow_id);
}

void FlowTracker::Begin(SliceId slice_id, FlowId flow_id) {
  BeginAt(Source(slice_id), flow_id);
}

void FlowTracker::BeginAt(FlowSource slice, FlowId flow_id) {
  auto it_and_ins = flow_to_slice_map_.Insert(flow_id, slice);
  if (!it_and_ins.second) {
    context_->stats_tracker->IncrementStats(stats::flow_duplicate_id);
    return;
  }
}

void FlowTracker::Step(TrackId track_id, FlowId flow_id) {
  auto open = context_->slice_tracker->GetTopmostOpenSlice(track_id);
  if (!open) {
    context_->stats_tracker->IncrementStats(stats::flow_no_enclosing_slice);
    return;
  }
  StepAt({open->id, open->ts}, flow_id);
}

void FlowTracker::Step(SliceId new_id, FlowId flow_id) {
  StepAt(Source(new_id), flow_id);
}

void FlowTracker::StepAt(FlowSource slice, FlowId flow_id) {
  auto* it = flow_to_slice_map_.Find(flow_id);
  if (!it) {
    context_->stats_tracker->IncrementStats(stats::flow_step_without_start);
    return;
  }
  SliceId existing_id = it->id;
  std::optional<int64_t> existing_ts = SourceTs(*it);
  std::optional<int64_t> new_ts = SourceTs(slice);
  SliceId outgoing = existing_id;
  SliceId incoming = slice.id;
  if (existing_ts && new_ts) {
    outgoing = *existing_ts > *new_ts ? slice.id : existing_id;
    incoming = *existing_ts <= *new_ts ? slice.id : existing_id;
  } else {
    context_->stats_tracker->IncrementStats(stats::flow_without_direction);
  }
  InsertFlow(flow_id, outgoing, incoming);
  *it = FlowSource{slice.id, new_ts};
}

void FlowTracker::End(TrackId track_id,
                      FlowId flow_id,
                      bool bind_enclosing_slice,
                      bool close_flow) {
  if (!bind_enclosing_slice) {
    pending_flow_ids_map_[track_id].push_back(flow_id);
    return;
  }
  auto open = context_->slice_tracker->GetTopmostOpenSlice(track_id);
  if (!open) {
    context_->stats_tracker->IncrementStats(stats::flow_no_enclosing_slice);
    return;
  }
  EndAt({open->id, open->ts}, flow_id, close_flow);
}

void FlowTracker::End(SliceId new_id, FlowId flow_id, bool close_flow) {
  EndAt(Source(new_id), flow_id, close_flow);
}

void FlowTracker::EndAt(FlowSource slice, FlowId flow_id, bool close_flow) {
  auto* it = flow_to_slice_map_.Find(flow_id);
  if (!it) {
    context_->stats_tracker->IncrementStats(stats::flow_end_without_start);
    return;
  }
  SliceId existing_id = it->id;
  std::optional<int64_t> existing_ts = SourceTs(*it);
  std::optional<int64_t> new_ts = SourceTs(slice);
  SliceId outgoing = existing_id;
  SliceId incoming = slice.id;
  if (existing_ts && new_ts) {
    outgoing = *existing_ts > *new_ts ? slice.id : existing_id;
    incoming = *existing_ts <= *new_ts ? slice.id : existing_id;
  } else {
    context_->stats_tracker->IncrementStats(stats::flow_without_direction);
  }
  if (close_flow)
    flow_to_slice_map_.Erase(flow_id);
  InsertFlow(flow_id, outgoing, incoming);
}

bool FlowTracker::IsActive(FlowId flow_id) const {
  return flow_to_slice_map_.Find(flow_id) != nullptr;
}

FlowId FlowTracker::GetFlowIdForV1Event(uint64_t source_id,
                                        StringId cat,
                                        StringId name) {
  V1FlowId v1_flow_id = {source_id, cat, name};
  auto* iter = v1_flow_id_to_flow_id_map_.Find(v1_flow_id);
  if (iter)
    return *iter;
  FlowId new_id = v1_id_counter_++;
  flow_id_to_v1_flow_id_map_[new_id] = v1_flow_id;
  v1_flow_id_to_flow_id_map_[v1_flow_id] = new_id;
  return new_id;
}

void FlowTracker::ClosePendingEventsOnTrack(TrackId track_id,
                                            SliceId slice_id) {
  auto* iter = pending_flow_ids_map_.Find(track_id);
  if (!iter)
    return;

  for (FlowId flow_id : *iter) {
    SliceId slice_out_id = flow_to_slice_map_[flow_id].id;
    InsertFlow(flow_id, slice_out_id, slice_id);
  }

  pending_flow_ids_map_.Erase(track_id);
}

void FlowTracker::InsertFlow(FlowId flow_id,
                             SliceId slice_out_id,
                             SliceId slice_in_id) {
  tables::FlowTable::Row row(slice_out_id, slice_in_id, flow_id, std::nullopt);
  auto id = context_->storage->mutable_flow_table()->Insert(row).id;

  auto* it = flow_id_to_v1_flow_id_map_.Find(flow_id);
  if (it) {
    // TODO(b/168007725): Add any args from v1 flow events and also export them.
    ArgsTracker args_tracker(context_);
    auto inserter = args_tracker.AddArgsTo(id);
    inserter.AddArg(name_key_id_, Variadic::String(it->name));
    inserter.AddArg(cat_key_id_, Variadic::String(it->cat));
  }
}

void FlowTracker::InsertFlow(SliceId slice_out_id, SliceId slice_in_id) {
  tables::FlowTable::Row row(slice_out_id, slice_in_id, std::nullopt,
                             std::nullopt);
  context_->storage->mutable_flow_table()->Insert(row);
}

}  // namespace trace_processor
}  // namespace perfetto
