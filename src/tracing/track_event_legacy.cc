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

#include "perfetto/tracing/track_event_legacy.h"

#include "perfetto/tracing/track.h"

namespace perfetto {
namespace legacy {

template <>
bool ConvertThreadId(const PerfettoLegacyCurrentThreadId&,
                     uint64_t*,
                     int32_t*,
                     int32_t*) {
  // No need to override anything for events on to the current thread.
  return false;
}

}  // namespace legacy

namespace internal {

void LegacyTraceId::Write(protos::pbzero::TrackEvent::LegacyEvent* event,
                          uint32_t event_flags) const {
  // Legacy flow events always use bind_id.
  if (event_flags & (TRACE_EVENT_FLAG_FLOW_OUT | TRACE_EVENT_FLAG_FLOW_IN)) {
    // Flow bind_ids don't have scopes, so we need to mangle in-process ones to
    // avoid collisions.
    if (id_flags_ & TRACE_EVENT_FLAG_HAS_LOCAL_ID) {
      event->set_bind_id(raw_id_ ^ ProcessTrack::Current().uuid);
    } else {
      event->set_bind_id(raw_id_);
    }
    return;
  }

  uint32_t scope_flags =
      id_flags_ & (TRACE_EVENT_FLAG_HAS_ID | TRACE_EVENT_FLAG_HAS_LOCAL_ID |
                   TRACE_EVENT_FLAG_HAS_GLOBAL_ID);
  switch (scope_flags) {
    case TRACE_EVENT_FLAG_HAS_ID:
      event->set_unscoped_id(raw_id_);
      break;
    case TRACE_EVENT_FLAG_HAS_LOCAL_ID:
      event->set_local_id(raw_id_);
      break;
    case TRACE_EVENT_FLAG_HAS_GLOBAL_ID:
      event->set_global_id(raw_id_);
      break;
  }
  if (scope_)
    event->set_id_scope(scope_);
}

}  // namespace internal
}  // namespace perfetto
