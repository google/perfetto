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

#include "src/bigtrace/orchestrator_impl.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/periodic_task.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/spawn.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/bigtrace/orchestrator.h"
#include "perfetto/ext/bigtrace/worker.h"
#include "protos/perfetto/bigtrace/orchestrator.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace bigtrace {
namespace {

base::Future<base::StatusOr<protos::TracePoolQueryResponse>>
RpcResponseToPoolResponse(base::StatusOr<protos::QueryTraceResponse> resp) {
  RETURN_IF_ERROR(resp.status());
  protos::TracePoolQueryResponse ret;
  ret.set_trace(std::move(resp->trace()));
  *ret.mutable_result() = std::move(*resp->mutable_result());
  return ret;
}

// The period of sync of state from the orchestrator to all the workers. This
// constant trades freshness (i.e. lower period) vs unnecessary work (i.e.
// higher period). 15s seems an acceptable number even for interactive trace
// loads.
static constexpr uint32_t kDefaultWorkerSyncPeriod = 15000;

}  // namespace

Orchestrator::~Orchestrator() = default;

std::unique_ptr<Orchestrator> Orchestrator::CreateInProcess(
    base::TaskRunner* task_runner,
    std::vector<std::unique_ptr<Worker>> workers) {
  return std::unique_ptr<Orchestrator>(
      new OrchestratorImpl(task_runner, std::move(workers)));
}

OrchestratorImpl::OrchestratorImpl(base::TaskRunner* task_runner,
                                   std::vector<std::unique_ptr<Worker>> workers)
    : task_runner_(task_runner),
      periodic_sync_task_(task_runner),
      workers_(std::move(workers)) {
  base::PeriodicTask::Args args;
  args.task = [this] { ExecuteSyncWorkers(); };
  args.period_ms = kDefaultWorkerSyncPeriod;
  args.start_first_task_immediately = true;
  periodic_sync_task_.Start(std::move(args));
}

base::StatusOrFuture<protos::TracePoolCreateResponse>
OrchestratorImpl::TracePoolCreate(const protos::TracePoolCreateArgs& args) {
  if (!args.has_pool_name()) {
    return base::StatusOr<protos::TracePoolCreateResponse>(
        base::ErrStatus("Pool name must be provided"));
  }
  std::string id = "stateless:" + args.pool_name();
  if (auto it_inserted = pools_.Insert(id, TracePool()); !it_inserted.second) {
    return base::StatusOr<protos::TracePoolCreateResponse>(
        base::ErrStatus("Pool '%s' already exists", id.c_str()));
  }
  return protos::TracePoolCreateResponse();
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
  if (!pool->traces.empty()) {
    return base::StatusOr<protos::TracePoolSetTracesResponse>(base::ErrStatus(
        "Incrementally adding/removing items to pool not currently supported"));
  }
  pool->traces.assign(args.traces().begin(), args.traces().end());

  uint32_t round_robin_worker_idx = 0;
  for (const std::string& trace_path : pool->traces) {
    auto it_and_inserted = traces_.Insert(trace_path, Trace());
    it_and_inserted.first->refcount++;
    if (it_and_inserted.second) {
      it_and_inserted.first->worker = workers_[round_robin_worker_idx].get();
      // Set the worker index to the next worker in a round-robin fashion.
      round_robin_worker_idx = (round_robin_worker_idx + 1) % workers_.size();
    } else {
      PERFETTO_CHECK(it_and_inserted.first);
    }
  }
  return protos::TracePoolSetTracesResponse();
}

base::StatusOrStream<protos::TracePoolQueryResponse>
OrchestratorImpl::TracePoolQuery(const protos::TracePoolQueryArgs& args) {
  TracePool* pool = pools_.Find(args.pool_id());
  if (!pool) {
    return base::StreamOf(base::StatusOr<protos::TracePoolQueryResponse>(
        base::ErrStatus("Unable to find pool %s", args.pool_id().c_str())));
  }

  std::vector<base::StatusOrStream<protos::QueryTraceResponse>> streams;
  protos::QueryTraceArgs query_args;
  *query_args.mutable_sql_query() = args.sql_query();
  for (const std::string& trace_path : pool->traces) {
    auto* trace = traces_.Find(trace_path);
    *query_args.mutable_trace() = trace_path;
    streams.emplace_back(trace->worker->QueryTrace(query_args));
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
  std::unordered_set<std::string> to_erase;
  for (auto it = traces_.GetIterator(); it; ++it) {
    PERFETTO_CHECK(it.value().refcount-- > 0);
    if (it.value().refcount == 0) {
      to_erase.emplace(it.key());
    }
  }
  for (const std::string& trace_path : to_erase) {
    traces_.Erase(trace_path);
  }
  PERFETTO_CHECK(pools_.Erase(id));
  return protos::TracePoolDestroyResponse();
}

void OrchestratorImpl::ExecuteSyncWorkers() {
  if (periodic_sync_handle_) {
    return;
  }
  periodic_sync_handle_ = base::SpawnFuture(task_runner_, [this]() {
    return SyncWorkers().ContinueWith([this](base::Status status) {
      if (!status.ok()) {
        PERFETTO_ELOG("%s", status.c_message());
      }
      periodic_sync_handle_ = std::nullopt;
      return base::Future<base::FVoid>(base::FVoid());
    });
  });
}

void OrchestratorImpl::ExecuteForceSyncWorkers() {
  // Destroy the sync handle to cancel any currently running sync.
  periodic_sync_handle_ = std::nullopt;
  ExecuteSyncWorkers();
}

base::StatusFuture OrchestratorImpl::SyncWorkers() {
  std::vector<base::StatusOrStream<protos::SyncTraceStateResponse>> streams;
  base::FlatHashMap<Worker*, std::vector<std::string>> worker_to_traces;
  for (auto it = traces_.GetIterator(); it; ++it) {
    auto it_and_inserted = worker_to_traces.Insert(it.value().worker, {});
    it_and_inserted.first->emplace_back(it.key());
  }
  for (auto& worker : workers_) {
    auto* traces = worker_to_traces.Find(worker.get());
    if (!traces) {
      continue;
    }
    protos::SyncTraceStateArgs args;
    for (const auto& trace : *traces) {
      args.add_traces(trace);
    }
    streams.push_back(worker->SyncTraceState(std::move(args)));
  }
  return base::FlattenStreams(std::move(streams))
      .MapFuture([](base::StatusOr<protos::SyncTraceStateResponse> resp) {
        return base::StatusFuture(resp.status());
      })
      .Collect(base::AllOkCollector());
}

}  // namespace bigtrace
}  // namespace perfetto
