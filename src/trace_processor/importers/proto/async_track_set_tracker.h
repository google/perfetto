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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ASYNC_TRACK_SET_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ASYNC_TRACK_SET_TRACKER_H_

#include <unordered_map>

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

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
//  TrackSetId set_id = track_set_tracker->InternAndroidSet(upid, name);
//  if (event.begin) {
//    TrackId id = track_set_tracker->Begin(set_id, cookie);
//    slice_tracker->Begin(ts, id, ...)
//  } else {
//    ... (same thing with end)
//  }
class AsyncTrackSetTracker {
 public:
  using TrackSetId = uint32_t;

  // Indicates the nesting behaviour of slices associated to a single
  // cookie.
  enum class NestingBehaviour {
    // Indicates that slices are unnestable; that is, it is an error
    // to call Begin -> Begin with a single cookie without End inbetween.
    // This pattern should be the default behaviour that most async slices
    // should use.
    kUnnestable,

    // Indicates that slices are unnestable but also saturating; that is
    // calling Begin -> Begin only causes a single Begin to be recorded.
    // This is only really useful for Android async slices which have this
    // behaviour for legacy reasons. See the comment in
    // SystraceParser::ParseSystracePoint for information on why
    // this behaviour exists.
    kLegacySaturatingUnnestable,
  };

  explicit AsyncTrackSetTracker(TraceProcessorContext* context);
  ~AsyncTrackSetTracker() = default;

  // Interns a set of Android async slice tracks assocaited with the given
  // upid and name.
  TrackSetId InternAndroidSet(UniquePid, StringId name);

  // Starts a new slice on the given async track set which has the given cookie
  // and nesting behaviour.
  TrackId Begin(TrackSetId id,
                int64_t cookie,
                NestingBehaviour = NestingBehaviour::kUnnestable);

  // Ends a new slice on the given async track set which has the given cookie
  // and nesting behaviour.
  TrackId End(TrackSetId id, int64_t cookie);

 private:
  struct AndroidTuple {
    UniquePid upid;
    StringId name;

    friend bool operator<(const AndroidTuple& l, const AndroidTuple& r) {
      return std::tie(l.upid, l.name) < std::tie(r.upid, r.name);
    }
  };

  enum class TrackSetType {
    kAndroid,
  };

  struct TrackState {
    TrackId id;
    int64_t cookie;
    uint32_t nest_count;
  };

  struct TrackSet {
    TrackSetType type;
    union {
      // Only set when |type| == |TrackSetType::kAndroid|.
      AndroidTuple android_tuple;
    };
    std::vector<TrackState> tracks;
  };

  // Gets an empty track (i.e. a track with no event started on it) or creates a
  // new track in the set if none exists.
  // Returns the index to the associated TrackState in the |tracks| vector.
  uint32_t GetOrCreateEmptyTrack(TrackSet& set, int64_t cookie);

  std::map<AndroidTuple, TrackSetId> android_track_set_ids_;
  std::vector<TrackSet> track_sets_;

  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ASYNC_TRACK_SET_TRACKER_H_
