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

namespace perfetto {
namespace trace_processor {

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : context_(context) {}

TrackId TrackTracker::InternFuchsiaAsyncTrack(
    const tables::FuchsiaAsyncTrackTable::Row& row) {
  FuchsiaAsyncTrackTuple tuple{row.correlation_id};

  auto it = fuchsia_async_tracks_.find(tuple);
  if (it != fuchsia_async_tracks_.end())
    return it->second;

  auto id = context_->storage->mutable_fuchsia_async_track_table()->Insert(row);
  fuchsia_async_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternGpuTrack(const tables::GpuTrackTable::Row& row) {
  GpuTrackTuple tuple{row.name.id, row.scope};

  auto it = gpu_tracks_.find(tuple);
  if (it != gpu_tracks_.end())
    return it->second;

  auto id = context_->storage->mutable_gpu_track_table()->Insert(row);
  gpu_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternChromeAsyncTrack(
    const tables::ChromeAsyncTrackTable::Row& row,
    StringId source_scope) {
  ChromeTrackTuple tuple;

  if (row.upid.has_value()) {
    tuple.scope = ChromeTrackTuple::Scope::kProcess;
    tuple.upid = row.upid;
  } else {
    tuple.scope = ChromeTrackTuple::Scope::kGlobal;
  }

  tuple.source_id = row.async_id;
  tuple.source_scope = source_scope;

  auto it = chrome_tracks_.find(tuple);
  if (it != chrome_tracks_.end())
    return it->second;

  auto id = context_->storage->mutable_chrome_async_track_table()->Insert(row);
  chrome_tracks_[tuple] = id;
  return id;
}

}  // namespace trace_processor
}  // namespace perfetto
