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

#ifndef SRC_BIGTRACE_ORCHESTRATOR_IMPL_H_
#define SRC_BIGTRACE_ORCHESTRATOR_IMPL_H_

#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/periodic_task.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/spawn.h"
#include "perfetto/ext/bigtrace/orchestrator.h"

namespace perfetto {
namespace protos {
class TracePoolShardCreateArgs;
}

namespace bigtrace {

class OrchestratorImpl : public Orchestrator {
 public:
  explicit OrchestratorImpl(base::TaskRunner*,
                            std::vector<std::unique_ptr<Worker>>);

  base::StatusOrStream<protos::TracePoolQueryResponse> TracePoolQuery(
      const protos::TracePoolQueryArgs&) override;

  base::StatusOrFuture<protos::TracePoolCreateResponse> TracePoolCreate(
      const protos::TracePoolCreateArgs&) override;

  base::StatusOrFuture<protos::TracePoolSetTracesResponse> TracePoolSetTraces(
      const protos::TracePoolSetTracesArgs&) override;

  base::StatusOrFuture<protos::TracePoolDestroyResponse> TracePoolDestroy(
      const protos::TracePoolDestroyArgs&) override;

 private:
  struct TracePool {
    std::vector<std::string> traces;
  };
  struct Trace {
    Worker* worker = nullptr;
    uint32_t refcount = 0;
  };
  void ExecuteSyncWorkers();
  void ExecuteForceSyncWorkers();
  base::StatusFuture SyncWorkers();

  base::TaskRunner* task_runner_ = nullptr;
  base::PeriodicTask periodic_sync_task_;
  std::optional<base::SpawnHandle> periodic_sync_handle_;

  std::vector<std::unique_ptr<Worker>> workers_;
  base::FlatHashMap<std::string, TracePool> pools_;
  base::FlatHashMap<std::string, Trace> traces_;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // SRC_BIGTRACE_ORCHESTRATOR_IMPL_H_
