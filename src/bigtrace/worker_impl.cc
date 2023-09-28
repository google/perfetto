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

#include "src/bigtrace/worker_impl.h"

#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/spawn.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/util.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/bigtrace/worker.h"
#include "protos/perfetto/bigtrace/orchestrator.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"
#include "src/bigtrace/trace_processor_wrapper.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace bigtrace {

Worker::~Worker() = default;

std::unique_ptr<Worker> Worker::CreateInProcesss(base::TaskRunner* runner,
                                                 Environment* environment,
                                                 base::ThreadPool* pool) {
  return std::make_unique<WorkerImpl>(runner, environment, pool);
}

WorkerImpl::WorkerImpl(base::TaskRunner* runner,
                       Environment* environment,
                       base::ThreadPool* pool)
    : task_runner_(runner), environment_(environment), thread_pool_(pool) {}

base::StatusOrStream<protos::SyncTraceStateResponse> WorkerImpl::SyncTraceState(
    const protos::SyncTraceStateArgs& args) {
  base::FlatHashMap<std::string, Trace> new_traces;
  std::vector<base::StatusStream> streams;
  for (const std::string& trace : args.traces()) {
    if (auto* ptr = traces_.Find(trace); ptr) {
      auto it_and_inserted = new_traces.Insert(trace, std::move(*ptr));
      PERFETTO_CHECK(it_and_inserted.second);
      continue;
    }
    auto [handle, stream] =
        base::SpawnResultFuture<base::Status>(task_runner_, [this, trace] {
          auto t = traces_.Find(trace);
          if (!t) {
            return base::StatusFuture(
                base::ErrStatus("%s: trace not found", trace.c_str()));
          }
          return t->wrapper->LoadTrace(environment_->ReadFile(trace));
        });
    auto tp = std::make_unique<TraceProcessorWrapper>(
        trace, thread_pool_, TraceProcessorWrapper::Statefulness::kStateless);
    streams.emplace_back(base::StreamFromFuture(std::move(stream)));
    new_traces.Insert(trace, Trace{std::move(tp), std::move(handle)});
  }
  traces_ = std::move(new_traces);
  return base::FlattenStreams(std::move(streams))
      .MapFuture([](base::Status status) {
        if (!status.ok()) {
          return base::StatusOrFuture<protos::SyncTraceStateResponse>(status);
        }
        return base::StatusOrFuture<protos::SyncTraceStateResponse>(
            protos::SyncTraceStateResponse());
      });
}

base::StatusOrStream<protos::QueryTraceResponse> WorkerImpl::QueryTrace(
    const protos::QueryTraceArgs& args) {
  auto* tp = traces_.Find(args.trace());
  if (!tp) {
    return base::StreamOf<base::StatusOr<protos::QueryTraceResponse>>(
        base::ErrStatus("%s: trace not found", args.trace().c_str()));
  }
  return tp->wrapper->Query(args.sql_query());
}

}  // namespace bigtrace
}  // namespace perfetto
