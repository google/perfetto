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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_INTERNAL_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_INTERNAL_H_

#include <unordered_map>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "perfetto/tracing/debug_annotation.h"
#include "perfetto/tracing/trace_writer_base.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
class EventContext;
namespace protos {
namespace pbzero {
class DebugAnnotation;
}  // namespace pbzero
}  // namespace protos

namespace internal {
class TrackEventCategoryRegistry;

class BaseTrackEventInternedDataIndex {
 public:
  virtual ~BaseTrackEventInternedDataIndex();

#if PERFETTO_DCHECK_IS_ON()
  const char* type_id_ = nullptr;
#endif  // PERFETTO_DCHECK_IS_ON()
};

struct TrackEventIncrementalState {
  static constexpr size_t kMaxInternedDataFields = 32;

  bool was_cleared = true;

  // A heap-allocated message for storing newly seen interned data while we are
  // in the middle of writing a track event. When a track event wants to write
  // new interned data into the trace, it is first serialized into this message
  // and then flushed to the real trace in EventContext when the packet ends.
  // The message is cached here as a part of incremental state so that we can
  // reuse the underlying buffer allocation for subsequently written interned
  // data.
  protozero::HeapBuffered<protos::pbzero::InternedData>
      serialized_interned_data;

  // In-memory indices for looking up interned data ids.
  // For each intern-able field (up to a max of 32) we keep a dictionary of
  // field-value -> interning-key. Depending on the type we either keep the full
  // value or a hash of it (See track_event_interned_data_index.h)
  using InternedDataIndex =
      std::pair</* interned_data.proto field number */ size_t,
                std::unique_ptr<BaseTrackEventInternedDataIndex>>;
  std::array<InternedDataIndex, kMaxInternedDataFields> interned_data_indices =
      {};
};

// The backend portion of the track event trace point implemention. Outlined to
// a separate .cc file so it can be shared by different track event category
// namespaces.
class TrackEventInternal {
 public:
  static bool Initialize(
      bool (*register_data_source)(const DataSourceDescriptor&));

  static void EnableTracing(const TrackEventCategoryRegistry& registry,
                            const DataSourceConfig& config,
                            uint32_t instance_index);
  static void DisableTracing(const TrackEventCategoryRegistry& registry,
                             uint32_t instance_index);

  static perfetto::EventContext WriteEvent(
      TraceWriterBase*,
      TrackEventIncrementalState*,
      const char* category,
      const char* name,
      perfetto::protos::pbzero::TrackEvent::Type);

  template <typename T>
  static void AddDebugAnnotation(perfetto::EventContext* event_ctx,
                                 const char* name,
                                 T&& value) {
    auto annotation = AddDebugAnnotation(event_ctx, name);
    WriteDebugAnnotation(annotation, value);
  }

 private:
  static protos::pbzero::DebugAnnotation* AddDebugAnnotation(
      perfetto::EventContext*,
      const char* name);
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_INTERNAL_H_
