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

#ifndef SRC_CLOUD_TRACE_PROCESSOR_ORCHESTRATOR_IMPL_H_
#define SRC_CLOUD_TRACE_PROCESSOR_ORCHESTRATOR_IMPL_H_

#include <memory>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/cloud_trace_processor/orchestrator.h"

namespace perfetto {
namespace protos {
class TracePoolShardCreateArgs;
}

namespace cloud_trace_processor {

class OrchestratorImpl : public Orchestrator {
 public:
  explicit OrchestratorImpl(std::vector<std::unique_ptr<Worker>> workers);

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
    std::vector<std::string> loaded_traces;
  };
  std::vector<std::unique_ptr<Worker>> workers_;
  base::FlatHashMap<std::string, TracePool> pools_;
};

}  // namespace cloud_trace_processor
}  // namespace perfetto

#endif  // SRC_CLOUD_TRACE_PROCESSOR_ORCHESTRATOR_IMPL_H_
