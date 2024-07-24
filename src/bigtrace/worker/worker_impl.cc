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
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto::bigtrace {

grpc::Status WorkerImpl::QueryTrace(
    grpc::ServerContext*,
    const protos::BigtraceQueryTraceArgs* args,
    protos::BigtraceQueryTraceResponse* response) {
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  base::Status status =
      trace_processor::ReadTrace(tp.get(), args->trace().c_str());
  if (!status.ok()) {
    const std::string& error_message = status.c_message();
    return grpc::Status(grpc::StatusCode::INTERNAL, error_message);
  }
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
