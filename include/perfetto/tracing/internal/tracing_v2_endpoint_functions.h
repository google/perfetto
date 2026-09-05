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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACING_V2_ENDPOINT_FUNCTIONS_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACING_V2_ENDPOINT_FUNCTIONS_H_

#include <memory>

#include "perfetto/base/export.h"

namespace perfetto {

namespace base {
class TaskRunner;
}

class ProducerEndpoint;

namespace internal {

// Keeps optional v2 symbols out of the always-linked muxer. Contains only
// operations that cannot go through ProducerEndpoint.
struct TracingV2EndpointFunctions {
  bool (*is_supported)() = nullptr;

  std::unique_ptr<ProducerEndpoint> (*create_endpoint)(
      std::unique_ptr<ProducerEndpoint>,
      base::TaskRunner* muxer_task_runner,
      base::TaskRunner* relay_task_runner) = nullptr;

  // Starts asynchronous release of the bridge's retained v1 TraceWriters.
  void (*start_releasing_v1_writers)(ProducerEndpoint*) = nullptr;

  // Returns whether the bridge still retains any v1 TraceWriter.
  bool (*has_retained_v1_writers)(const ProducerEndpoint*) = nullptr;
};

PERFETTO_EXPORT_COMPONENT const TracingV2EndpointFunctions*
GetTracingV2EndpointFunctions();

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACING_V2_ENDPOINT_FUNCTIONS_H_
