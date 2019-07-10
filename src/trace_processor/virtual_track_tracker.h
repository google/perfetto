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

#ifndef SRC_TRACE_PROCESSOR_VIRTUAL_TRACK_TRACKER_H_
#define SRC_TRACE_PROCESSOR_VIRTUAL_TRACK_TRACKER_H_

#include "perfetto/protozero/field.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores virtual tracks by their source IDs.
class VirtualTrackTracker {
 public:
  explicit VirtualTrackTracker(TraceProcessorContext*);

  struct SourceIdTuple {
    VirtualTrackScope scope;
    // Only relevant if scope is kProcess. Otherwise, should be set to 0.
    UniquePid upid;
    int64_t source_id;
    StringId source_id_scope;

    friend bool operator<(const SourceIdTuple& l, const SourceIdTuple& r) {
      return std::tie(l.scope, l.upid, l.source_id, l.source_id_scope) <
             std::tie(r.scope, r.upid, r.source_id, r.source_id_scope);
    }
  };

  // Returns the TrackId of the virtual track with the provided |SourceIdTuple|.
  // If no virtual track for the provided ID tuple exists yet, creates a new
  // virtual track for and assigns the provided |track_name| to it. |track_name|
  // is ignored otherwise.
  TrackId GetOrCreateTrack(SourceIdTuple, StringId track_name);

 private:
  std::map<SourceIdTuple, TrackId> tracks_;
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VIRTUAL_TRACK_TRACKER_H_
