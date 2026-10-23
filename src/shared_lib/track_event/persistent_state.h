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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_PERSISTENT_STATE_H_
#define SRC_SHARED_LIB_TRACK_EVENT_PERSISTENT_STATE_H_

#include <cstdint>

#include "perfetto/public/abi/track_event_hl_abi.h"

// Persistent state tracks.
//
// While a category is enabled, each of its persistent state tracks owns a heap
// allocated snapshot of its current state: the start timestamp and the event
// fields (state name and extra args) pre-serialized without interning. All the
// emitted events are regular "forward looking" state events, whose timestamp is
// the start of the state:
// * When the state changes, the previous state is emitted: this is when it
//   ends, so it survives ring buffer wraparound as long as possible.
// * When a data source instance is flushed, the states that changed since the
//   last flush are emitted (so that write-into-file traces see them within the
//   sorting window). The first, clone and stop flushes emit all the states (so
//   that unchanged states are visible).
// * When the last instance with a category enabled stops, the snapshots are
//   freed.

namespace perfetto::shlib {

// Handles a PERFETTO_TE_TYPE_STATE event with a
// PERFETTO_TE_HL_EXTRA_TYPE_PERSISTENT_STATE_TRACK extra.
void PersistentStateTrackUpdate(
    struct PerfettoTeCategoryImpl* cat,
    const struct PerfettoTeHlExtraPersistentStateTrack& extra,
    const char* name,
    struct PerfettoTeHlExtra* const* extra_data);

// Emits the persistent states for the data source instance `instance_index`:
// all of them if `all` is true, otherwise only the ones that changed since the
// last call.
void PersistentStateTracksFlush(uint32_t instance_index, bool all);

// Frees the state of the tracks whose category is not enabled anymore.
void PersistentStateTracksOnStop();

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_PERSISTENT_STATE_H_
