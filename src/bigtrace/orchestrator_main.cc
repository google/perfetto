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
#include <memory>

#include <grpcpp/client_context.h>
#include <grpcpp/grpcpp.h>

#include "perfetto/base/status.h"
#include "protos/perfetto/bigtrace/orchestrator.grpc.pb.h"
#include "protos/perfetto/bigtrace/orchestrator.pb.h"
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"

namespace perfetto {
namespace bigtrace {
namespace {

class OrchestratorImpl final : public protos::BigtraceOrchestrator::Service {
  grpc::Status Query(
      grpc::ServerContext*,
      const protos::BigtraceQueryArgs*,
      grpc::ServerWriter<protos::BigtraceQueryResponse>*) override {
    return grpc::Status::OK;
  }
};

base::Status OrchestratorMain(int, char**) {
  // Setup the Orchestrator Server
  std::string server_address("localhost:5051");
  auto service = std::make_unique<OrchestratorImpl>();
  grpc::ServerBuilder builder;
  builder.RegisterService(service.get());
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  // Setup the Orchestrator Client
  std::string target_address("localhost:5052");
  auto channel =
      grpc::CreateChannel(target_address, grpc::InsecureChannelCredentials());
  bool connected = channel->WaitForConnected(std::chrono::system_clock::now() +
                                             std::chrono::milliseconds(5000));

  PERFETTO_CHECK(connected);

  std::string example_trace = "test/data/api34_startup_cold.perfetto-trace";
  std::string example_query = "SELECT * FROM slice";

  auto stub = protos::BigtraceWorker::NewStub(channel);
  grpc::ClientContext context;
  protos::BigtraceQueryTraceArgs args;
  protos::BigtraceQueryTraceResponse response;

  args.set_trace(example_trace);
  args.set_sql_query(example_query);

  grpc::Status status = stub->QueryTrace(&context, args, &response);

  if (status.ok()) {
    PERFETTO_LOG("Received response with result_size: %i",
                 response.result_size());
  } else {
    PERFETTO_LOG("Failed to query trace");
  }

  // Build and start the Orchestrator server
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  PERFETTO_LOG("Orchestrator server listening on %s", server_address.c_str());

  server->Wait();

  return base::OkStatus();
}

}  // namespace
}  // namespace bigtrace
}  // namespace perfetto

int main(int argc, char** argv) {
  auto status = perfetto::bigtrace::OrchestratorMain(argc, argv);
  if (!status.ok()) {
    fprintf(stderr, "%s\n", status.c_message());
    return 1;
  }
  return 0;
}
