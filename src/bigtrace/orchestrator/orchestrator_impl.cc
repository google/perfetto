/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <chrono>
#include <mutex>
#include <thread>

#include "perfetto/base/logging.h"
#include "src/bigtrace/orchestrator/orchestrator_impl.h"

namespace perfetto::bigtrace {

namespace {
const uint32_t kBufferPushDelay = 100;
}

OrchestratorImpl::OrchestratorImpl(
    std::unique_ptr<protos::BigtraceWorker::Stub> stub,
    uint32_t pool_size)
    : stub_(std::move(stub)),
      pool_(std::make_unique<base::ThreadPool>(pool_size)),
      semaphore_(pool_size) {}

grpc::Status OrchestratorImpl::Query(
    grpc::ServerContext*,
    const protos::BigtraceQueryArgs* args,
    grpc::ServerWriter<protos::BigtraceQueryResponse>* writer) {
  grpc::Status query_status;
  std::mutex status_lock;
  const std::string& sql_query = args->sql_query();

  std::vector<protos::BigtraceQueryResponse> response_buffer;
  uint64_t trace_count = static_cast<uint64_t>(args->traces_size());

  std::thread push_response_buffer_thread([&]() {
    uint64_t pushed_response_count = 0;
    for (;;) {
      {
        std::lock_guard<std::mutex> status_guard(status_lock);
        if (pushed_response_count == trace_count || !query_status.ok()) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kBufferPushDelay));
      if (response_buffer.empty()) {
        continue;
      }
      std::vector<protos::BigtraceQueryResponse> buffer;
      {
        std::lock_guard<std::mutex> buffer_guard(buffer_lock_);
        buffer = std::move(response_buffer);
        response_buffer.clear();
      }
      for (protos::BigtraceQueryResponse& response : buffer) {
        writer->Write(std::move(response));
      }
      pushed_response_count += buffer.size();
    }
  });

  for (const std::string& trace : args->traces()) {
    {
      std::lock_guard<std::mutex> status_guard(status_lock);
      if (!query_status.ok()) {
        break;
      }
    }
    semaphore_.Acquire();
    pool_->PostTask([&]() {
      grpc::ClientContext client_context;
      protos::BigtraceQueryTraceArgs trace_args;
      protos::BigtraceQueryTraceResponse trace_response;

      trace_args.set_sql_query(sql_query);
      trace_args.set_trace(trace);
      grpc::Status status =
          stub_->QueryTrace(&client_context, trace_args, &trace_response);
      if (!status.ok()) {
        PERFETTO_ELOG("QueryTrace returned an error status %s",
                      status.error_message().c_str());
        {
          std::lock_guard<std::mutex> status_guard(status_lock);
          query_status = status;
        }
      } else {
        protos::BigtraceQueryResponse response;
        response.set_trace(trace_response.trace());
        for (const protos::QueryResult& query_result :
             trace_response.result()) {
          response.add_result()->CopyFrom(query_result);
        }
        std::lock_guard<std::mutex> buffer_guard(buffer_lock_);
        response_buffer.emplace_back(std::move(response));
      }
      semaphore_.Release();
    });
  }
  push_response_buffer_thread.join();
  return query_status;
}

void OrchestratorImpl::Semaphore::Acquire() {
  std::unique_lock<std::mutex> lk(mutex_);
  while (!count_) {
    cv_.wait(lk);
  }
  --count_;
}

void OrchestratorImpl::Semaphore::Release() {
  std::lock_guard<std::mutex> lk(mutex_);
  ++count_;
  cv_.notify_one();
}

}  // namespace perfetto::bigtrace
