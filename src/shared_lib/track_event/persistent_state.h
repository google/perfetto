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

// Persistent state tracks (see PerfettoTePersistentStateTrack).
//
// Each track always owns a heap allocated copy of its current state: the time
// it started and the event fields (value and args), serialized without
// interning. All the emitted events are regular state events, whose timestamp
// is the start of the state:
// * When the state changes, both the previous state and the new state are
//   emitted. The previous state is written when it ends, so that it survives
//   ring buffer wraparound as long as possible.
// * When a data source instance starts, stops or is cloned, all the states are
//   emitted on that instance.
//
// Changing the state doesn't take any lock. A state is only accessed by
// whoever took it out of its track with an atomic exchange: a snapshot takes
// the state out, emits it and puts it back.

namespace perfetto::shlib {

// Emits the current state of all the persistent state tracks whose category is
// enabled on the data source instance `instance_index`, only on that instance.
void EmitPersistentStates(uint32_t instance_index);

}  // namespace perfetto::shlib

#endif  // SRC_SHARED_LIB_TRACK_EVENT_PERSISTENT_STATE_H_
