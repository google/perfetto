/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BIGTRACE_WORKER_H_
#define INCLUDE_PERFETTO_EXT_BIGTRACE_WORKER_H_

#include <memory>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/stream.h"

namespace perfetto {

namespace base {
class ThreadPool;
}

namespace protos {
class SyncTraceStateArgs;
class SyncTraceStateResponse;

class QueryTraceArgs;
class QueryTraceResponse;
}  // namespace protos

namespace bigtrace {

class Environment;

// Interface for a BigTrace "Worker".
//
// See BigTraceWorker RPC service for high-level documentation.
class Worker {
 public:
  virtual ~Worker();

  // Returns an in-process implementation of the Worker given an instance of
  // |Environment| and a |ThreadPool|. The |Environment| will be used to
  // perform any interaction with the OS (e.g. opening and reading files) and
  // the |ThreadPool| will be used to dispatch requests to TraceProcessor.
  static std::unique_ptr<Worker> CreateInProcesss(base::TaskRunner*,
                                                  Environment*,
                                                  base::ThreadPool*);

  // Synchronize the state of the traces in the worker to the orchestrator.
  virtual base::StatusOrStream<protos::SyncTraceStateResponse> SyncTraceState(
      const protos::SyncTraceStateArgs&) = 0;

  // Executes a SQL query on the specified trace.
  virtual base::StatusOrStream<protos::QueryTraceResponse> QueryTrace(
      const protos::QueryTraceArgs&) = 0;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BIGTRACE_WORKER_H_
