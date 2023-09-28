/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BIGTRACE_ORCHESTRATOR_H_
#define INCLUDE_PERFETTO_EXT_BIGTRACE_ORCHESTRATOR_H_

#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/stream.h"

namespace perfetto {
namespace protos {
class TracePoolCreateArgs;
class TracePoolCreateResponse;

class TracePoolSetTracesArgs;
class TracePoolSetTracesResponse;

class TracePoolQueryArgs;
class TracePoolQueryResponse;

class TracePoolDestroyArgs;
class TracePoolDestroyResponse;
}  // namespace protos
}  // namespace perfetto

namespace perfetto {
namespace bigtrace {

class Worker;

// Interface for a BigTrace "Orchestrator".
//
// See BigTraceOrchestrator RPC service for high-level documentation.
class Orchestrator {
 public:
  virtual ~Orchestrator();

  // Returns an in-process implementation of the Orchestrator, given a group of
  // workers which can be delegated to.
  //
  // Note that the passed workers instances can be "remote" (i.e. in another
  // process or even on another machine); the returned manager will gracefully
  // handle this.
  static std::unique_ptr<Orchestrator> CreateInProcess(
      base::TaskRunner*,
      std::vector<std::unique_ptr<Worker>> workers);

  // Creates a TracePool with the specified arguments.
  virtual base::StatusOrFuture<protos::TracePoolCreateResponse> TracePoolCreate(
      const protos::TracePoolCreateArgs&) = 0;

  // Associates the provided list of traces to this TracePoolShard.
  virtual base::StatusOrFuture<protos::TracePoolSetTracesResponse>
  TracePoolSetTraces(const protos::TracePoolSetTracesArgs&) = 0;

  // Executes a SQL query on the specified TracePool.
  virtual base::StatusOrStream<protos::TracePoolQueryResponse> TracePoolQuery(
      const protos::TracePoolQueryArgs&) = 0;

  // Destroys the TracePool with the specified id.
  virtual base::StatusOrFuture<protos::TracePoolDestroyResponse>
  TracePoolDestroy(const protos::TracePoolDestroyArgs&) = 0;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BIGTRACE_ORCHESTRATOR_H_
