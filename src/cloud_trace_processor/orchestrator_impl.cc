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

#include "src/cloud_trace_processor/orchestrator_impl.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/cloud_trace_processor/worker.h"
#include "protos/perfetto/cloud_trace_processor/common.pb.h"
#include "protos/perfetto/cloud_trace_processor/orchestrator.pb.h"
#include "protos/perfetto/cloud_trace_processor/worker.pb.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace cloud_trace_processor {
namespace {

base::Future<base::Status> CreateResponseToStatus(
    base::StatusOr<protos::TracePoolShardCreateResponse> response_or) {
  return response_or.status();
}

base::Future<base::Status> SetTracesResponseToStatus(
    base::StatusOr<protos::TracePoolShardSetTracesResponse> response_or) {
  return response_or.status();
}

base::Future<base::StatusOr<protos::TracePoolQueryResponse>>
RpcResponseToPoolResponse(
    base::StatusOr<protos::TracePoolShardQueryResponse> resp) {
  RETURN_IF_ERROR(resp.status());
  protos::TracePoolQueryResponse ret;
  ret.set_trace(std::move(resp->trace()));
  *ret.mutable_result() = std::move(*resp->mutable_result());
  return ret;
}

base::StatusOrStream<protos::TracePoolShardSetTracesResponse>
RoundRobinSetTraces(const std::vector<std::unique_ptr<Worker>>& workers,
                    const std::vector<std::string>& traces) {
  uint32_t worker_idx = 0;
  std::vector<protos::TracePoolShardSetTracesArgs> protos;
  protos.resize(workers.size());
  for (const auto& trace : traces) {
    protos[worker_idx].add_traces(trace);
    worker_idx = (worker_idx + 1) % workers.size();
  }

  using ShardResponse = protos::TracePoolShardSetTracesResponse;
  std::vector<base::StatusOrStream<ShardResponse>> streams;
  for (uint32_t i = 0; i < protos.size(); ++i) {
    streams.emplace_back(workers[i]->TracePoolShardSetTraces(protos[i]));
  }
  return base::FlattenStreams(std::move(streams));
}
}  // namespace

Orchestrator::~Orchestrator() = default;

std::unique_ptr<Orchestrator> Orchestrator::CreateInProcess(
    std::vector<std::unique_ptr<Worker>> workers) {
  return std::unique_ptr<Orchestrator>(
      new OrchestratorImpl(std::move(workers)));
}

OrchestratorImpl::OrchestratorImpl(std::vector<std::unique_ptr<Worker>> workers)
    : workers_(std::move(workers)) {}

base::StatusOrFuture<protos::TracePoolCreateResponse>
OrchestratorImpl::TracePoolCreate(const protos::TracePoolCreateArgs& args) {
  if (args.pool_type() != protos::TracePoolType::SHARED) {
    return base::StatusOr<protos::TracePoolCreateResponse>(
        base::ErrStatus("Currently only SHARED pools are supported"));
  }
  if (!args.has_shared_pool_name()) {
    return base::StatusOr<protos::TracePoolCreateResponse>(
        base::ErrStatus("Pool name must be provided for SHARED pools"));
  }

  std::string id = "shared:" + args.shared_pool_name();
  TracePool* exist = pools_.Find(id);
  if (exist) {
    return base::StatusOr<protos::TracePoolCreateResponse>(
        base::ErrStatus("Pool %s already exists", id.c_str()));
  }
  protos::TracePoolShardCreateArgs group_args;
  group_args.set_pool_id(id);
  group_args.set_pool_type(args.pool_type());

  using ShardResponse = protos::TracePoolShardCreateResponse;
  std::vector<base::StatusOrStream<ShardResponse>> shards;
  for (uint32_t i = 0; i < workers_.size(); ++i) {
    shards.emplace_back(
        base::StreamFromFuture(workers_[i]->TracePoolShardCreate(group_args)));
  }
  return base::FlattenStreams(std::move(shards))
      .MapFuture(&CreateResponseToStatus)
      .Collect(base::AllOkCollector())
      .ContinueWith(
          [this, id](base::StatusOr<ShardResponse> resp)
              -> base::StatusOrFuture<protos::TracePoolCreateResponse> {
            RETURN_IF_ERROR(resp.status());
            auto it_and_inserted = pools_.Insert(id, TracePool());
            if (!it_and_inserted.second) {
              return base::ErrStatus("Unable to insert pool %s", id.c_str());
            }
            return protos::TracePoolCreateResponse();
          });
}

base::StatusOrFuture<protos::TracePoolSetTracesResponse>
OrchestratorImpl::TracePoolSetTraces(
    const protos::TracePoolSetTracesArgs& args) {
  std::string id = args.pool_id();
  TracePool* pool = pools_.Find(id);
  if (!pool) {
    return base::StatusOr<protos::TracePoolSetTracesResponse>(
        base::ErrStatus("Unable to find pool %s", id.c_str()));
  }
  if (!pool->loaded_traces.empty()) {
    return base::StatusOr<protos::TracePoolSetTracesResponse>(base::ErrStatus(
        "Incrementally adding/removing items to pool not currently supported"));
  }
  pool->loaded_traces.assign(args.traces().begin(), args.traces().end());
  return RoundRobinSetTraces(workers_, pool->loaded_traces)
      .MapFuture(&SetTracesResponseToStatus)
      .Collect(base::AllOkCollector())
      .ContinueWith(
          [](base::Status status)
              -> base::StatusOrFuture<protos::TracePoolSetTracesResponse> {
            RETURN_IF_ERROR(status);
            return protos::TracePoolSetTracesResponse();
          });
}

base::StatusOrStream<protos::TracePoolQueryResponse>
OrchestratorImpl::TracePoolQuery(const protos::TracePoolQueryArgs& args) {
  TracePool* pool = pools_.Find(args.pool_id());
  if (!pool) {
    return base::StreamOf(base::StatusOr<protos::TracePoolQueryResponse>(
        base::ErrStatus("Unable to find pool %s", args.pool_id().c_str())));
  }
  protos::TracePoolShardQueryArgs shard_args;
  *shard_args.mutable_pool_id() = args.pool_id();
  *shard_args.mutable_sql_query() = args.sql_query();

  using ShardResponse = protos::TracePoolShardQueryResponse;
  std::vector<base::StatusOrStream<ShardResponse>> streams;
  for (uint32_t i = 0; i < workers_.size(); ++i) {
    streams.emplace_back(workers_[i]->TracePoolShardQuery(shard_args));
  }
  return base::FlattenStreams(std::move(streams))
      .MapFuture(&RpcResponseToPoolResponse);
}

base::StatusOrFuture<protos::TracePoolDestroyResponse>
OrchestratorImpl::TracePoolDestroy(const protos::TracePoolDestroyArgs& args) {
  std::string id = args.pool_id();
  TracePool* pool = pools_.Find(id);
  if (!pool) {
    return base::StatusOr<protos::TracePoolDestroyResponse>(
        base::ErrStatus("Unable to find pool %s", id.c_str()));
  }
  protos::TracePoolShardDestroyArgs shard_args;
  *shard_args.mutable_pool_id() = id;

  using ShardResponse = protos::TracePoolShardDestroyResponse;
  std::vector<base::StatusOrStream<ShardResponse>> streams;
  for (uint32_t i = 0; i < workers_.size(); ++i) {
    streams.emplace_back(
        base::StreamFromFuture(workers_[i]->TracePoolShardDestroy(shard_args)));
  }
  return base::FlattenStreams(std::move(streams))
      .MapFuture(
          [](base::StatusOr<ShardResponse> resp) -> base::Future<base::Status> {
            return resp.status();
          })
      .Collect(base::AllOkCollector())
      .ContinueWith(
          [this, id](base::Status status)
              -> base::StatusOrFuture<protos::TracePoolDestroyResponse> {
            RETURN_IF_ERROR(status);
            PERFETTO_CHECK(pools_.Erase(id));
            return protos::TracePoolDestroyResponse();
          });
}

}  // namespace cloud_trace_processor
}  // namespace perfetto
