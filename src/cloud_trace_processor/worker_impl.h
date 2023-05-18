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

#ifndef SRC_CLOUD_TRACE_PROCESSOR_WORKER_IMPL_H_
#define SRC_CLOUD_TRACE_PROCESSOR_WORKER_IMPL_H_

#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/cloud_trace_processor/environment.h"
#include "perfetto/ext/cloud_trace_processor/worker.h"
#include "src/cloud_trace_processor/trace_processor_wrapper.h"

namespace perfetto {
namespace protos {

enum GroupType : int;

}  // namespace protos
}  // namespace perfetto

namespace perfetto {
namespace cloud_trace_processor {

class WorkerImpl : public Worker {
 public:
  explicit WorkerImpl(CtpEnvironment*, base::ThreadPool*);

  base::StatusOrFuture<protos::TracePoolShardCreateResponse>
  TracePoolShardCreate(const protos::TracePoolShardCreateArgs&) override;

  base::StatusOrStream<protos::TracePoolShardSetTracesResponse>
  TracePoolShardSetTraces(const protos::TracePoolShardSetTracesArgs&) override;

  base::StatusOrStream<protos::TracePoolShardQueryResponse> TracePoolShardQuery(
      const protos::TracePoolShardQueryArgs&) override;

  base::StatusOrFuture<protos::TracePoolShardDestroyResponse>
  TracePoolShardDestroy(const protos::TracePoolShardDestroyArgs&) override;

 private:
  struct TracePoolShard {
    std::vector<std::unique_ptr<TraceProcessorWrapper>> tps;
  };
  CtpEnvironment* const environment_;
  base::ThreadPool* const thread_pool_;
  base::FlatHashMap<std::string, TracePoolShard> shards_;
};

}  // namespace cloud_trace_processor
}  // namespace perfetto

#endif  // SRC_CLOUD_TRACE_PROCESSOR_WORKER_IMPL_H_
