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

#include "src/trace_processor/virtual_track_tracker.h"

namespace perfetto {
namespace trace_processor {

VirtualTrackTracker::VirtualTrackTracker(TraceProcessorContext* context)
    : context_(context) {}

TrackId VirtualTrackTracker::GetOrCreateTrack(SourceIdTuple id_tuple,
                                              StringId track_name) {
  PERFETTO_DCHECK(id_tuple.scope == VirtualTrackScope::kProcess ||
                  id_tuple.upid == 0);

  auto it = tracks_.find(id_tuple);
  if (it != tracks_.end())
    return it->second;

  TrackId track_id = context_->storage->mutable_tracks()->AddTrack(track_name);
  context_->storage->mutable_virtual_tracks()->AddVirtualTrack(
      track_id, id_tuple.scope, id_tuple.upid);
  tracks_[id_tuple] = track_id;
  return track_id;
}

}  // namespace trace_processor
}  // namespace perfetto
