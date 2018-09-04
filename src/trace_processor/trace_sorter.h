/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
#define SRC_TRACE_PROCESSOR_TRACE_SORTER_H_

#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Events from the trace come into this class ordered per cpu. This class stores
// the events for |window_size_ns| ns and then outputs all the collected events
// in the correct global order.
// When |window_size_ns| == 0 this class will push packets directly to be parsed
// and stored. This means no ordering can occur.
class TraceSorter {
 public:
  struct TimestampedTracePiece {
    TimestampedTracePiece(TraceBlobView bv, bool is_f, uint32_t c)
        : blob_view(std::move(bv)), is_ftrace(is_f), cpu(c) {}

    TraceBlobView blob_view;
    bool is_ftrace;
    uint32_t cpu;
  };
  using EventsMap = std::multimap<uint64_t /*ts*/, TimestampedTracePiece>;

  TraceSorter(TraceProcessorContext*, uint64_t window_size_ns);

  void PushTracePacket(uint64_t timestamp, TraceBlobView);
  void PushFtracePacket(uint32_t cpu, uint64_t timestamp, TraceBlobView);

  // This method passes any events older than window_size_ns to the
  // parser to be parsed and then stored.
  void MaybeFlushEvents();

  // Flush all events ignorinig the window.
  void FlushEventsForced();

  void set_window_ns_for_testing(uint64_t window_size_ns) {
    window_size_ns_ = window_size_ns;
  }

 private:
  TraceProcessorContext* const context_;
  uint64_t window_size_ns_;
  EventsMap events_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
