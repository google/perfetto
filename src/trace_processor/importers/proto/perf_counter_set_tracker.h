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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PERF_COUNTER_SET_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PERF_COUNTER_SET_TRACKER_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Tracks perf counter sets and allocates set IDs.
// A counter set groups multiple counter values (timebase + followers)
// that were recorded at the same sample point.
class PerfCounterSetTracker {
 public:
  explicit PerfCounterSetTracker(TraceProcessorContext* context);

  // Adds a counter set containing the given counter IDs.
  // Returns the set ID that can be stored in PerfSampleTable.
  uint32_t AddCounterSet(const std::vector<CounterId>& counter_ids);

 private:
  TraceProcessorContext* context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PERF_COUNTER_SET_TRACKER_H_
