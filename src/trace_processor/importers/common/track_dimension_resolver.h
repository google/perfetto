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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_DIMENSION_RESOLVER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_DIMENSION_RESOLVER_H_

namespace perfetto::trace_processor {

class TraceProcessorContext;

// Turns the dimension *declarations* recorded during import
// (`__intrinsic_track_dimension_decl`) into the *effective* dimensions of
// every track (`__intrinsic_track_dimension`).
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
// This runs once, globally, after all events have been extracted: it is a
// whole-trace pass and must see every track.
void ResolveTrackDimensions(TraceProcessorContext* context);

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_TRACK_DIMENSION_RESOLVER_H_
