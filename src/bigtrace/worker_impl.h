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

#ifndef SRC_BIGTRACE_WORKER_IMPL_H_
#define SRC_BIGTRACE_WORKER_IMPL_H_

#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/threading/spawn.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/bigtrace/environment.h"
#include "perfetto/ext/bigtrace/worker.h"
#include "src/bigtrace/trace_processor_wrapper.h"

namespace perfetto {
namespace protos {

enum GroupType : int;

}  // namespace protos
}  // namespace perfetto

namespace perfetto {
namespace bigtrace {

class WorkerImpl : public Worker {
 public:
  explicit WorkerImpl(base::TaskRunner*, Environment*, base::ThreadPool*);

  // Synchronize the state of the traces in the worker to the orchestrator.
  base::StatusOrStream<protos::SyncTraceStateResponse> SyncTraceState(
      const protos::SyncTraceStateArgs&) override;

  // Executes a SQL query on the specified trace.
  base::StatusOrStream<protos::QueryTraceResponse> QueryTrace(
      const protos::QueryTraceArgs&) override;

 private:
  struct Trace {
    std::unique_ptr<TraceProcessorWrapper> wrapper;
    base::SpawnHandle load_handle;
  };
  base::TaskRunner* const task_runner_;
  Environment* const environment_;
  base::ThreadPool* const thread_pool_;
  base::FlatHashMap<std::string, Trace> traces_;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // SRC_BIGTRACE_WORKER_IMPL_H_
