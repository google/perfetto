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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_POLICY_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_POLICY_H_

#include <cstdint>

namespace perfetto::trace_processor::core::exec {

// Layout adaptation never pulls more input. Any accepts the producer's views;
// PreferContiguous requests packing of scattered columns for cheaper reads.
// A selection within one vector-sized span can still be served as a view.
enum class LayoutPreference : uint8_t { kAny, kPreferContiguous };

// Throughput permits combining small batches, subject to execution demand.
// Neither preference permits changing row order.
enum class BatchPreference : uint8_t { kLatency, kThroughput };

struct InputPolicy {
  LayoutPreference layout = LayoutPreference::kAny;
  BatchPreference batching = BatchPreference::kLatency;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_POLICY_H_
