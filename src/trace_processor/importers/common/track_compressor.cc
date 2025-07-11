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

#include "src/trace_processor/importers/common/track_compressor.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

TrackCompressor::TrackCompressor(TraceProcessorContext* context)
    : context_(context) {}

std::pair<TrackId*, uint32_t> TrackCompressor::BeginInternal(
    TrackSet& set,
    NestingBehaviour nesting_behaviour,
    int64_t cookie) {
  uint32_t idx = GetOrCreateTrackForCookie(set.tracks, cookie);
  TrackState& state = set.tracks[idx];
  switch (nesting_behaviour) {
    case NestingBehaviour::kNestable:
      state.nest_count++;
      break;
    case NestingBehaviour::kLegacySaturatingUnnestable:
      PERFETTO_DCHECK(state.nest_count <= 1);
      state.nest_count = 1;
      break;
  }
  return std::make_pair(&state.track_id, idx);
}

std::pair<TrackId*, uint32_t> TrackCompressor::EndInternal(TrackSet& set,
                                                           int64_t cookie) {
  uint32_t idx = GetOrCreateTrackForCookie(set.tracks, cookie);
  TrackState& state = set.tracks[idx];

  // It's possible to have a nest count of 0 even when we know about the track.
  // Suppose the following sequence of events for some |id| and |cookie|:
  //   Begin
  //   (trace starts)
  //   Begin
  //   End
  //   End <- nest count == 0 here even though we have a record of this track.
  if (state.nest_count > 0)
    state.nest_count--;
  return std::make_pair(&state.track_id, idx);
}

std::pair<TrackId*, uint32_t> TrackCompressor::ScopedInternal(TrackSet& set,
                                                              int64_t ts,
                                                              int64_t dur) {
  auto it = std::find_if(
      set.tracks.begin(), set.tracks.end(), [ts](const TrackState& state) {
        return state.slice_type == TrackState::SliceType::kTimestamp &&
               state.ts_end <= ts;
      });
  if (it != set.tracks.end()) {
    it->ts_end = ts + dur;
    return std::make_pair(&it->track_id, static_cast<uint32_t>(std::distance(
                                             set.tracks.begin(), it)));
  }

  TrackState state;
  state.slice_type = TrackState::SliceType::kTimestamp;
  state.ts_end = ts + dur;
  state.track_id = kInvalidTrackId;
  set.tracks.emplace_back(state);

  uint32_t idx = static_cast<uint32_t>(set.tracks.size() - 1);
  return std::make_pair(&set.tracks.back().track_id, idx);
}

uint32_t TrackCompressor::GetOrCreateTrackForCookie(
    std::vector<TrackState>& tracks,
    int64_t cookie) {
  auto it = std::find_if(
      tracks.begin(), tracks.end(), [cookie](const TrackState& state) {
        return state.slice_type == TrackState::SliceType::kCookie &&
               state.cookie == cookie;
      });
  if (it != tracks.end())
    return static_cast<uint32_t>(std::distance(tracks.begin(), it));

  it = std::find_if(tracks.begin(), tracks.end(), [](const TrackState& state) {
    return state.slice_type == TrackState::SliceType::kCookie &&
           state.nest_count == 0;
  });
  if (it != tracks.end()) {
    // Adopt this track for the cookie to make sure future slices with this
    // cookie also get associated to this track.
    it->cookie = cookie;
    return static_cast<uint32_t>(std::distance(tracks.begin(), it));
  }

  TrackState state;
  state.slice_type = TrackState::SliceType::kCookie;
  state.cookie = cookie;
  state.nest_count = 0;
  state.track_id = kInvalidTrackId;
  tracks.emplace_back(state);

  return static_cast<uint32_t>(tracks.size() - 1);
}

}  // namespace perfetto::trace_processor
