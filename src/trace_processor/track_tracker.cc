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

#include "src/trace_processor/track_tracker.h"

#include "src/trace_processor/args_tracker.h"

namespace perfetto {
namespace trace_processor {

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : source_key_(context->storage->InternString("source")),
      source_id_key_(context->storage->InternString("source_id")),
      source_scope_key_(context->storage->InternString("source_scope")),
      fuchsia_source_(context->storage->InternString("fuchsia")),
      chrome_source_(context->storage->InternString("chrome")),
      context_(context) {}

TrackId TrackTracker::InternFuchsiaAsyncTrack(StringId name,
                                              int64_t correlation_id) {
  FuchsiaAsyncTrackTuple tuple{correlation_id};

  auto it = fuchsia_async_tracks_.find(tuple);
  if (it != fuchsia_async_tracks_.end())
    return it->second;

  tables::TrackTable::Row row(name);
  auto id = context_->storage->mutable_track_table()->Insert(row);
  fuchsia_async_tracks_[tuple] = id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(fuchsia_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(correlation_id));
  return id;
}

TrackId TrackTracker::InternGpuTrack(const tables::GpuTrackTable::Row& row) {
  GpuTrackTuple tuple{row.name.id, row.scope, row.context_id.value_or(0)};

  auto it = gpu_tracks_.find(tuple);
  if (it != gpu_tracks_.end())
    return it->second;

  auto id = context_->storage->mutable_gpu_track_table()->Insert(row);
  gpu_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternChromeTrack(StringId name,
                                        base::Optional<uint32_t> upid,
                                        int64_t source_id,
                                        StringId source_scope) {
  ChromeTrackTuple tuple;
  tuple.upid = upid;
  tuple.source_id = source_id;
  tuple.source_scope = source_scope;

  auto it = chrome_tracks_.find(tuple);
  if (it != chrome_tracks_.end())
    return it->second;

  TrackId id;
  if (upid.has_value()) {
    tables::ProcessTrackTable::Row track(name);
    track.upid = *upid;
    id = context_->storage->mutable_process_track_table()->Insert(track);
  } else {
    tables::TrackTable::Row track(name);
    id = context_->storage->mutable_track_table()->Insert(track);
  }
  chrome_tracks_[tuple] = id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(chrome_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(source_id));
  context_->args_tracker->AddArg(row_id, source_scope_key_, source_scope_key_,
                                 Variadic::String(source_scope));
  return id;
}

}  // namespace trace_processor
}  // namespace perfetto
