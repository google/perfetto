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

#include "src/trace_processor/importers/common/async_track_set_tracker.h"

#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

AsyncTrackSetTracker::AsyncTrackSetTracker(TraceProcessorContext* context)
    : android_source_(context->storage->InternString("android")),
      context_(context) {}

AsyncTrackSetTracker::TrackSetId AsyncTrackSetTracker::InternGlobalTrackSet(
    StringId name) {
  auto it = global_track_set_ids_.find(name);
  if (it != global_track_set_ids_.end()) {
    return it->second;
  }

  uint32_t id = static_cast<uint32_t>(track_sets_.size());
  TrackSet set;
  set.global_track_name = name;
  set.scope = TrackSetScope::kGlobal;
  set.nesting_behaviour = NestingBehaviour::kNestable;
  track_sets_.emplace_back(set);

  return global_track_set_ids_[name] = id;
}

AsyncTrackSetTracker::TrackSetId AsyncTrackSetTracker::InternProcessTrackSet(
    UniquePid upid,
    StringId name) {
  ProcessTuple tuple{upid, name};

  auto it = process_track_set_ids_.find(tuple);
  if (it != process_track_set_ids_.end())
    return it->second;

  uint32_t id = static_cast<uint32_t>(track_sets_.size());

  TrackSet set;
  set.process_tuple = tuple;
  set.scope = TrackSetScope::kProcess;
  set.nesting_behaviour = NestingBehaviour::kNestable;
  track_sets_.emplace_back(set);

  process_track_set_ids_[tuple] = id;
  return id;
}

AsyncTrackSetTracker::TrackSetId
AsyncTrackSetTracker::InternAndroidLegacyUnnestableTrackSet(UniquePid upid,
                                                            StringId name) {
  ProcessTuple tuple{upid, name};

  auto it = android_legacy_unnestable_track_set_ids_.find(tuple);
  if (it != android_legacy_unnestable_track_set_ids_.end())
    return it->second;

  uint32_t id = static_cast<uint32_t>(track_sets_.size());

  TrackSet set;
  set.process_tuple = tuple;
  set.scope = TrackSetScope::kProcess;
  set.nesting_behaviour = NestingBehaviour::kLegacySaturatingUnnestable;
  track_sets_.emplace_back(set);

  android_legacy_unnestable_track_set_ids_[tuple] = id;
  return id;
}

TrackId AsyncTrackSetTracker::Begin(TrackSetId id, int64_t cookie) {
  PERFETTO_DCHECK(id < track_sets_.size());

  TrackSet& set = track_sets_[id];
  TrackState& state = GetOrCreateTrackForCookie(set, cookie);
  switch (set.nesting_behaviour) {
    case NestingBehaviour::kNestable:
      state.nest_count++;
      break;
    case NestingBehaviour::kLegacySaturatingUnnestable:
      PERFETTO_DCHECK(state.nest_count <= 1);
      state.nest_count = 1;
      break;
  }
  return state.id;
}

TrackId AsyncTrackSetTracker::End(TrackSetId id, int64_t cookie) {
  PERFETTO_DCHECK(id < track_sets_.size());

  TrackSet& set = track_sets_[id];
  TrackState& state = GetOrCreateTrackForCookie(set, cookie);

  // It's possible to have a nest count of 0 even when we know about the track.
  // Suppose the following sequence of events for some |id| and |cookie|:
  //   Begin
  //   (trace starts)
  //   Begin
  //   End
  //   End <- nest count == 0 here even though we have a record of this track.
  if (state.nest_count > 0)
    state.nest_count--;
  return state.id;
}

TrackId AsyncTrackSetTracker::Scoped(TrackSetId id, int64_t ts, int64_t dur) {
  PERFETTO_DCHECK(id < track_sets_.size());

  TrackSet& set = track_sets_[id];
  PERFETTO_DCHECK(set.nesting_behaviour !=
                  NestingBehaviour::kLegacySaturatingUnnestable);

  auto it = std::find_if(
      set.tracks.begin(), set.tracks.end(), [ts](const TrackState& state) {
        return state.slice_type == TrackState::SliceType::kTimestamp &&
               state.ts_end <= ts;
      });
  if (it != set.tracks.end()) {
    it->ts_end = ts + dur;
    return it->id;
  }

  TrackState state;
  state.slice_type = TrackState::SliceType::kTimestamp;
  state.ts_end = ts + dur;
  state.id = CreateTrackForSet(set);
  set.tracks.emplace_back(state);

  return state.id;
}

AsyncTrackSetTracker::TrackState&
AsyncTrackSetTracker::GetOrCreateTrackForCookie(TrackSet& set, int64_t cookie) {
  auto it = std::find_if(
      set.tracks.begin(), set.tracks.end(), [cookie](const TrackState& state) {
        return state.slice_type == TrackState::SliceType::kCookie &&
               state.cookie == cookie;
      });
  if (it != set.tracks.end())
    return *it;

  it = std::find_if(
      set.tracks.begin(), set.tracks.end(), [](const TrackState& state) {
        return state.slice_type == TrackState::SliceType::kCookie &&
               state.nest_count == 0;
      });
  if (it != set.tracks.end()) {
    // Adopt this track for the cookie to make sure future slices with this
    // cookie also get associated to this track.
    it->cookie = cookie;
    return *it;
  }

  TrackState state;
  state.id = CreateTrackForSet(set);
  state.slice_type = TrackState::SliceType::kCookie;
  state.cookie = cookie;
  state.nest_count = 0;
  set.tracks.emplace_back(state);

  return set.tracks.back();
}

TrackId AsyncTrackSetTracker::CreateTrackForSet(const TrackSet& set) {
  switch (set.scope) {
    case TrackSetScope::kGlobal:
      // TODO(lalitm): propogate source from callers rather than just passing
      // kNullStringId here.
      return context_->track_tracker->LegacyCreateGlobalAsyncTrack(
          set.global_track_name, kNullStringId);
    case TrackSetScope::kProcess:
      // TODO(lalitm): propogate source from callers rather than just passing
      // kNullStringId here.
      StringId source =
          set.nesting_behaviour == NestingBehaviour::kLegacySaturatingUnnestable
              ? android_source_
              : kNullStringId;
      return context_->track_tracker->LegacyCreateProcessAsyncTrack(
          set.process_tuple.name, set.process_tuple.upid, source);
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace trace_processor
}  // namespace perfetto
