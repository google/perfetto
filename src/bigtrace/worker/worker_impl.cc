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

#include "src/bigtrace/worker/worker_impl.h"
#include "perfetto/ext/trace_processor/rpc/query_result_serializer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/bigtrace/worker/repository_policies/gcs_trace_processor_loader.h"
#include "src/bigtrace/worker/repository_policies/local_trace_processor_loader.h"

namespace perfetto::bigtrace {

grpc::Status WorkerImpl::QueryTrace(
    grpc::ServerContext*,
    const protos::BigtraceQueryTraceArgs* args,
    protos::BigtraceQueryTraceResponse* response) {
  std::string args_trace = args->trace();

  std::string prefix = args_trace.substr(0, args_trace.find("/", 1));
  if (registry_.find(prefix) == registry_.end()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Invalid path prefix specified");
  }

  if (prefix.length() == args_trace.length()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "Empty path is invalid");
  }

  std::string path = args_trace.substr(prefix.length() + 1);

  base::StatusOr<std::unique_ptr<trace_processor::TraceProcessor>> tp_or =
      registry_[prefix]->LoadTraceProcessor(path);

  if (!tp_or.ok()) {
    const std::string& error_message = tp_or.status().message();
    return grpc::Status(grpc::StatusCode::INTERNAL, error_message);
  }

  std::unique_ptr<trace_processor::TraceProcessor> tp = std::move(*tp_or);

  auto iter = tp->ExecuteQuery(args->sql_query());
  trace_processor::QueryResultSerializer serializer =
      trace_processor::QueryResultSerializer(std::move(iter));

  std::vector<uint8_t> serialized;
  for (bool has_more = true; has_more;) {
    serialized.clear();
    has_more = serializer.Serialize(&serialized);
    response->add_result()->ParseFromArray(serialized.data(),
                                           static_cast<int>(serialized.size()));
  }
  response->set_trace(args->trace());

  return grpc::Status::OK;
}

}  // namespace perfetto::bigtrace
