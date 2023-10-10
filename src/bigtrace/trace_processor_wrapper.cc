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

#include "src/bigtrace/trace_processor_wrapper.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/future.h"
#include "perfetto/ext/base/threading/poll.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/threading/util.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/bigtrace/worker.pb.h"
#include "src/protozero/proto_ring_buffer.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace bigtrace {
namespace {

using trace_processor::QueryResultSerializer;
using trace_processor::TraceBlob;
using trace_processor::TraceBlobView;
using trace_processor::TraceProcessor;
using Statefulness = TraceProcessorWrapper::Statefulness;

struct QueryRunner {
  QueryRunner(std::shared_ptr<TraceProcessor> _tp,
              std::string _query,
              std::string _trace_path,
              Statefulness _statefulness)
      : tp(std::move(_tp)),
        query(std::move(_query)),
        trace_path(std::move(_trace_path)),
        statefulness(_statefulness) {}

  std::optional<protos::QueryTraceResponse> operator()() {
    if (!has_more) {
      if (statefulness == Statefulness::kStateless) {
        tp->RestoreInitialTables();
      }
      return std::nullopt;
    }
    // If the serializer does not exist yet, that means we have not yet run
    // the query so make sure to do that first.
    EnsureSerializerExists();
    has_more = serializer->Serialize(&result);

    protos::QueryTraceResponse resp;
    *resp.mutable_trace() = trace_path;
    resp.mutable_result()->ParseFromArray(result.data(),
                                          static_cast<int>(result.size()));
    result.clear();
    return std::make_optional(std::move(resp));
  }

  void EnsureSerializerExists() {
    if (serializer) {
      return;
    }
    auto it = tp->ExecuteQuery(query);
    serializer.reset(new QueryResultSerializer(std::move(it)));
  }

  std::shared_ptr<TraceProcessor> tp;
  std::string query;
  std::string trace_path;
  TraceProcessorWrapper::Statefulness statefulness;

  // shared_ptr to allow copying when this type is coerced to std::function.
  std::shared_ptr<QueryResultSerializer> serializer;
  std::vector<uint8_t> result;
  bool has_more = true;
};

}  // namespace

TraceProcessorWrapper::TraceProcessorWrapper(std::string trace_path,
                                             base::ThreadPool* thread_pool,
                                             Statefulness statefulness)
    : trace_path_(std::move(trace_path)),
      thread_pool_(thread_pool),
      statefulness_(statefulness) {
  trace_processor::Config config;
  config.ingest_ftrace_in_raw_table = false;
  trace_processor_ = TraceProcessor::CreateInstance(config);
}

base::StatusFuture TraceProcessorWrapper::LoadTrace(
    base::StatusOrStream<std::vector<uint8_t>> file_stream) {
  if (trace_processor_.use_count() != 1) {
    return base::ErrStatus("Request is already in flight");
  }
  return std::move(file_stream)
      .MapFuture(
          [thread_pool = thread_pool_, tp = trace_processor_](
              base::StatusOr<std::vector<uint8_t>> d) -> base::StatusFuture {
            RETURN_IF_ERROR(d.status());
            return base::RunOnceOnThreadPool<base::Status>(
                thread_pool, [res = std::move(*d), tp = std::move(tp)] {
                  return tp->Parse(TraceBlobView(
                      TraceBlob::CopyFrom(res.data(), res.size())));
                });
          })
      .Collect(base::AllOkCollector())
      .ContinueWith([thread_pool = thread_pool_, tp = trace_processor_](
                        base::Status status) -> base::StatusFuture {
        RETURN_IF_ERROR(status);
        return base::RunOnceOnThreadPool<base::Status>(
            thread_pool, [tp = std::move(tp)] {
              tp->NotifyEndOfFile();
              return base::OkStatus();
            });
      });
}

base::StatusOrStream<protos::QueryTraceResponse> TraceProcessorWrapper::Query(
    const std::string& query) {
  using StatusOrResponse = base::StatusOr<protos::QueryTraceResponse>;
  if (trace_processor_.use_count() != 1) {
    return base::StreamOf<StatusOrResponse>(
        base::ErrStatus("Request is already in flight"));
  }
  return base::RunOnThreadPool<StatusOrResponse>(
      thread_pool_,
      QueryRunner(trace_processor_, query, trace_path_, statefulness_),
      [tp = trace_processor_] { tp->InterruptQuery(); });
}

}  // namespace bigtrace
}  // namespace perfetto
