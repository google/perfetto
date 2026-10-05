/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_DIMENSION_RESOLVER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_DIMENSION_RESOLVER_H_

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/track_tables_py.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Turns the custom dimensions declared on TrackDescriptors (see
// TrackDescriptor.dimensions) into the *effective* dimensions of every track
// (`__intrinsic_track_dimension`).
//
// A declaration applies to:
//  1. every track associated with the same `upid`, if it was declared on a
//     root process track (this includes the process' threads),
//  2. every track associated with the same `utid`, if it was declared on a
//     root thread track,
//  3. the declaring track and its `parent_id` descendants otherwise.
//
// Repeating the same name and value is deduplicated. Declaring a *different*
// value for an inherited name is invalid producer input: the inherited value
// wins and `track_dimension_conflicting_value` is recorded.
//
// This is owned by TrackEventTracker and only knows about the declarations it
// was given. Resolution happens once all events have been extracted, so it
// covers every track created while parsing (including tracks created by other
// importers for a process/thread which declared dimensions).
class TrackEventDimensionResolver {
 public:
  // A custom dimension. Exactly one of |int_value| / |string_value| is set.
  struct Dimension {
    StringId name = kNullStringId;
    std::optional<int64_t> int_value;
    std::optional<StringId> string_value;
    std::optional<StringId> display_name;
  };
  using DimensionVec = std::vector<Dimension>;

  explicit TrackEventDimensionResolver(TraceProcessorContext*);
  ~TrackEventDimensionResolver();

  TrackEventDimensionResolver(const TrackEventDimensionResolver&) = delete;
  TrackEventDimensionResolver& operator=(const TrackEventDimensionResolver&) =
      delete;

  // Records |dims| as declared on the root track of a process, thread or on
  // any other track respectively.
  void DeclareForProcess(UniquePid, const DimensionVec& dims);
  void DeclareForThread(UniqueTid, const DimensionVec& dims);
  void DeclareForTrack(TrackId, const DimensionVec& dims);

  // Writes the effective dimensions of every track affected by the
  // declarations into `__intrinsic_track_dimension`. Must be called once, after
  // all events have been extracted.
  void ResolveAll();

 private:
  const DimensionVec& Resolve(TrackId);

  TraceProcessorContext* const context_;
  TraceStorage* const storage_;

  std::unordered_map<uint32_t, DimensionVec> by_upid_;
  std::unordered_map<uint32_t, DimensionVec> by_utid_;
  std::unordered_map<uint32_t, DimensionVec> by_track_;
  std::unordered_map<uint32_t, DimensionVec> resolved_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_DIMENSION_RESOLVER_H_
