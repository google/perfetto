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

#include "src/cloud_trace_processor/worker_impl.h"

#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/uuid.h"
#include "protos/perfetto/cloud_trace_processor/common.pb.h"
#include "protos/perfetto/cloud_trace_processor/orchestrator.pb.h"
#include "protos/perfetto/cloud_trace_processor/worker.pb.h"
#include "src/cloud_trace_processor/trace_processor_wrapper.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace cloud_trace_processor {

Worker::~Worker() = default;

std::unique_ptr<Worker> Worker::CreateInProcesss(CtpEnvironment* environment,
                                                 base::ThreadPool* pool) {
  return std::make_unique<WorkerImpl>(environment, pool);
}

WorkerImpl::WorkerImpl(CtpEnvironment* environment, base::ThreadPool* pool)
    : environment_(environment), thread_pool_(pool) {}

base::StatusOrFuture<protos::TracePoolShardCreateResponse>
WorkerImpl::TracePoolShardCreate(const protos::TracePoolShardCreateArgs& args) {
  if (args.pool_type() == protos::TracePoolType::DEDICATED) {
    return base::ErrStatus("Dedicated pools are not currently supported");
  }
  auto it_and_inserted = shards_.Insert(args.pool_id(), TracePoolShard());
  if (!it_and_inserted.second) {
    return base::ErrStatus("Shard for pool %s already exists",
                           args.pool_id().c_str());
  }
  return base::StatusOr(protos::TracePoolShardCreateResponse());
}

base::StatusOrStream<protos::TracePoolShardSetTracesResponse>
WorkerImpl::TracePoolShardSetTraces(
    const protos::TracePoolShardSetTracesArgs& args) {
  using Response = protos::TracePoolShardSetTracesResponse;
  using StatusOrResponse = base::StatusOr<Response>;

  TracePoolShard* shard = shards_.Find(args.pool_id());
  if (!shard) {
    return base::StreamOf<StatusOrResponse>(base::ErrStatus(
        "Unable to find shard for pool %s", args.pool_id().c_str()));
  }

  std::vector<base::StatusOrStream<Response>> streams;
  for (const std::string& trace : args.traces()) {
    // TODO(lalitm): add support for stateful trace processor in dedicated
    // pools.
    auto tp = std::make_unique<TraceProcessorWrapper>(
        trace, thread_pool_, TraceProcessorWrapper::Statefulness::kStateless);
    auto load_trace_future =
        tp->LoadTrace(environment_->ReadFile(trace))
            .ContinueWith(
                [trace](base::Status status) -> base::Future<StatusOrResponse> {
                  RETURN_IF_ERROR(status);
                  protos::TracePoolShardSetTracesResponse resp;
                  *resp.mutable_trace() = trace;
                  return resp;
                });
    streams.emplace_back(base::StreamFromFuture(std::move(load_trace_future)));
    shard->tps.emplace_back(std::move(tp));
  }
  return base::FlattenStreams(std::move(streams));
}

base::StatusOrStream<protos::TracePoolShardQueryResponse>
WorkerImpl::TracePoolShardQuery(const protos::TracePoolShardQueryArgs& args) {
  using Response = protos::TracePoolShardQueryResponse;
  using StatusOrResponse = base::StatusOr<Response>;
  TracePoolShard* shard = shards_.Find(args.pool_id());
  if (!shard) {
    return base::StreamOf<StatusOrResponse>(base::ErrStatus(
        "Unable to find shard for pool %s", args.pool_id().c_str()));
  }
  std::vector<base::StatusOrStream<Response>> streams;
  streams.reserve(shard->tps.size());
  for (std::unique_ptr<TraceProcessorWrapper>& tp : shard->tps) {
    streams.emplace_back(tp->Query(args.sql_query()));
  }
  return base::FlattenStreams(std::move(streams));
}

base::StatusOrFuture<protos::TracePoolShardDestroyResponse>
WorkerImpl::TracePoolShardDestroy(
    const protos::TracePoolShardDestroyArgs& args) {
  if (!shards_.Erase(args.pool_id())) {
    return base::ErrStatus("Unable to find shard for pool %s",
                           args.pool_id().c_str());
  }
  return base::StatusOr(protos::TracePoolShardDestroyResponse());
}

}  // namespace cloud_trace_processor
}  // namespace perfetto
