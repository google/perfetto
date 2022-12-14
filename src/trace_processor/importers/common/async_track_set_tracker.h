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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ASYNC_TRACK_SET_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ASYNC_TRACK_SET_TRACKER_H_

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;
class AsyncTrackSetTrackerUnittest;

// Tracker used to reduce the number of trace processor tracks corresponding
// to a single "UI track".
//
// UIs using trace processor want to display all slices in the same context
// (e.g. same upid) and same name into a single track. However, because trace
// processor does not allow parallel slices on a single track (because it breaks
// things like span join, self time computation etc.), at the trace processor
// level these parallel slices are put on different tracks.
//
// Creating a new track for every event, however, leads to an explosion of
// tracks which is undesirable. This class exists to multiplex slices so that
// n events correspond to a single track in a way which minimises the number of
// tracks which needs to be merged by the UI.
//
// The intended usage of this class is for callers to first call one of the
// Intern* methods to obtain a TrackSetId followed by Begin/End just before
// calling into SliceTracker's Begin/End respectively. For example:
//  TrackSetId set_id = track_set_tracker->InternProcessTrackSet(upid, name);
//  if (event.begin) {
//    TrackId id = track_set_tracker->Begin(set_id, cookie);
//    slice_tracker->Begin(ts, id, ...)
//  } else {
//    ... (same thing with end)
//  }
// Alternatively, instead of Begin/End, Scoped can also be called if supported
// by the track type.
class AsyncTrackSetTracker {
 public:
  using TrackSetId = uint32_t;

  explicit AsyncTrackSetTracker(TraceProcessorContext* context);
  ~AsyncTrackSetTracker() = default;

  // Interns a set of global async slice tracks associated with the given name.
  TrackSetId InternGlobalTrackSet(StringId name);

  // Interns a set of process async slice tracks associated with the given name
  // and upid.
  TrackSetId InternProcessTrackSet(UniquePid, StringId name);

  // Interns a set of Android legacy unnesteable async slice tracks
  // associated with the given upid and name.
  // Scoped is *not* supported for this track set type.
  TrackSetId InternAndroidLegacyUnnestableTrackSet(UniquePid, StringId name);

  // Starts a new slice on the given async track set which has the given cookie.
  TrackId Begin(TrackSetId id, int64_t cookie);

  // Ends a new slice on the given async track set which has the given cookie.
  TrackId End(TrackSetId id, int64_t cookie);

  // Creates a scoped slice on the given async track set.
  // This method makes sure that any other slice in this track set does
  // not happen simultaneously on the returned track.
  // Only supported on selected track set types; read the documentation for
  // the Intern* method for your track type to check if supported.
  TrackId Scoped(TrackSetId id, int64_t ts, int64_t dur);

 private:
  friend class AsyncTrackSetTrackerUnittest;

  struct ProcessTuple {
    UniquePid upid;
    StringId name;

    friend bool operator<(const ProcessTuple& l, const ProcessTuple& r) {
      return std::tie(l.upid, l.name) < std::tie(r.upid, r.name);
    }
  };

  // Indicates the nesting behaviour of slices associated to a single slice
  // stack.
  enum class NestingBehaviour {
    // Indicates that slices are nestable; that is, a stack of slices with
    // the same cookie should stack correctly (but are not allowed to overlap).
    // This pattern should be the default behaviour that most async slices
    // should use.
    kNestable,

    // Indicates that slices are unnestable but also saturating; that is
    // calling Begin -> Begin only causes a single Begin to be recorded.
    // This is only really useful for Android async slices which have this
    // behaviour for legacy reasons. See the comment in
    // SystraceParser::ParseSystracePoint for information on why
    // this behaviour exists.
    kLegacySaturatingUnnestable,
  };

  enum class TrackSetScope {
    kGlobal,
    kProcess,
  };

  struct TrackState {
    TrackId id;

    enum class SliceType { kCookie, kTimestamp };
    SliceType slice_type;

    union {
      // Only valid for |slice_type| == |SliceType::kCookie|.
      int64_t cookie;

      // Only valid for |slice_type| == |SliceType::kTimestamp|.
      int64_t ts_end;
    };

    // Only used for |slice_type| == |SliceType::kCookie|.
    uint32_t nest_count;
  };

  struct TrackSet {
    TrackSetScope scope;
    union {
      // Only set when |scope| == |TrackSetScope::kGlobal|.
      StringId global_track_name;
      // Only set when |scope| == |TrackSetScope::kFrameTimeline| or
      // |TrackSetScope::kAndroidLegacyUnnestable|.
      ProcessTuple process_tuple;
    };
    NestingBehaviour nesting_behaviour;
    std::vector<TrackState> tracks;
  };

  // Returns the state for a track using the following algorithm:
  // 1. If a track exists with the given cookie in the track set, returns
  //    that track.
  // 2. Otherwise, looks for any track in the set which is "open" (i.e.
  //    does not have another slice currently scheduled).
  // 3. Otherwise, creates a new track and associates it with the set.
  TrackState& GetOrCreateTrackForCookie(TrackSet& set, int64_t cookie);

  TrackId CreateTrackForSet(const TrackSet& set);

  std::map<StringId, TrackSetId> global_track_set_ids_;
  std::map<ProcessTuple, TrackSetId> process_track_set_ids_;
  std::map<ProcessTuple, TrackSetId> android_legacy_unnestable_track_set_ids_;
  std::vector<TrackSet> track_sets_;

  const StringId android_source_ = kNullStringId;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ASYNC_TRACK_SET_TRACKER_H_
