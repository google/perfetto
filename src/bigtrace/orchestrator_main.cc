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
#include <grpcpp/impl/service_type.h>
#include <grpcpp/support/status.h>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/getopt.h"
#include "protos/perfetto/bigtrace/orchestrator.grpc.pb.h"
#include "protos/perfetto/bigtrace/orchestrator.pb.h"
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"

namespace perfetto {
namespace bigtrace {
namespace {

struct CommandLineOptions {
  std::string worker_address;
};

CommandLineOptions ParseCommandLineOptions(int argc, char** argv) {
  CommandLineOptions command_line_options;
  static option long_options[] = {{"worker", required_argument, nullptr, 'w'},
                                  {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "w:", long_options, nullptr)) != -1) {
    switch (c) {
      case 'w':
        command_line_options.worker_address = optarg;
        break;
      default:
        PERFETTO_ELOG("Usage: %s --worker=worker_address", argv[0]);
        break;
    }
  }

  return command_line_options;
}

class OrchestratorImpl final : public protos::BigtraceOrchestrator::Service {
 public:
  explicit OrchestratorImpl(std::unique_ptr<protos::BigtraceWorker::Stub> _stub)
      : stub(std::move(_stub)) {}

 private:
  std::unique_ptr<protos::BigtraceWorker::Stub> stub;
  grpc::Status Query(
      grpc::ServerContext*,
      const protos::BigtraceQueryArgs* args,
      grpc::ServerWriter<protos::BigtraceQueryResponse>* writer) override {
    const std::string& sql_query = args->sql_query();
    for (const std::string& trace : args->traces()) {
      grpc::ClientContext client_context;
      protos::BigtraceQueryTraceArgs trace_args;
      protos::BigtraceQueryTraceResponse trace_response;

      trace_args.set_sql_query(sql_query);
      trace_args.set_trace(trace);
      grpc::Status status =
          stub->QueryTrace(&client_context, trace_args, &trace_response);
      if (!status.ok()) {
        return status;
      }
      protos::BigtraceQueryResponse response;
      response.set_trace(trace_response.trace());
      for (const protos::QueryResult& query_result : trace_response.result()) {
        response.add_result()->CopyFrom(query_result);
      }
      writer->Write(response);
    }
    return grpc::Status::OK;
  }
};

base::Status OrchestratorMain(int argc, char** argv) {
  CommandLineOptions options = ParseCommandLineOptions(argc, argv);

  std::string server_address("localhost:5051");
  std::string worker_address = !options.worker_address.empty()
                                   ? options.worker_address
                                   : "localhost:5052";

  // Setup the Orchestrator Client
  auto channel =
      grpc::CreateChannel(worker_address, grpc::InsecureChannelCredentials());
  bool connected = channel->WaitForConnected(std::chrono::system_clock::now() +
                                             std::chrono::milliseconds(5000));

  PERFETTO_CHECK(connected);

  auto stub = protos::BigtraceWorker::NewStub(channel);
  auto service = std::make_unique<OrchestratorImpl>(std::move(stub));

  // Setup the Orchestrator Server
  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(service.get());
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
