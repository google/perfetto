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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_

#include "src/trace_processor/importers/proto/typed_proto_field.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

// A single out-of-tree extension field (`extensions 1000 to 9999`) of a
// TrackEvent, handed to plugins for inspection. The plugin decodes it with the
// generated field metadata of the extension it owns, e.g.
//   field.Cast<FrameworksBaseTrackEvent::kProcessStart>()  // -> ConstBytes
using TrackEventExtensionField = TypedProtoField;

// Observes TrackEvent extension fields once the core slice/counter/state row
// has been inserted. A plugin registers (via TrackEventParser) the extension
// field ids it owns; each id is owned by exactly one plugin. Only modern
// counter/slice/state events are dispatched, never legacy ones.
class TrackEventPlugin {
 public:
  // kHandled means the parser skips the default flattening of the event into
  // the args table; kIgnored leaves it untouched.
  enum class Result { kHandled, kIgnored };

  virtual ~TrackEventPlugin();

  // Default to kIgnored so a plugin only overrides the kinds it handles.
  virtual Result OnCounter(const TrackEventExtensionField& field, CounterId id);
  virtual Result OnSlice(const TrackEventExtensionField& field, SliceId id);
  virtual Result OnState(const TrackEventExtensionField& field, StateId id);
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
