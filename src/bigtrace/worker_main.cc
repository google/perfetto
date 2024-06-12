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

#include <grpcpp/grpcpp.h>

#include "perfetto/base/status.h"
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"

namespace perfetto {
namespace bigtrace {
namespace {

class WorkerImpl final : public protos::BigtraceWorker::Service {
  grpc::Status QueryTrace(grpc::ServerContext*,
                          const protos::BigtraceQueryTraceArgs*,
                          protos::BigtraceQueryTraceResponse*) override {
    return grpc::Status::OK;
  }
};

base::Status WorkerMain(int, char**) {
  // Setup the Worker Server
  std::string server_address("localhost:5052");
  auto service = std::make_unique<WorkerImpl>();
  grpc::ServerBuilder builder;
  builder.RegisterService(service.get());
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  PERFETTO_LOG("Worker server listening on %s", server_address.c_str());

  server->Wait();

  return base::OkStatus();
}

}  // namespace
}  // namespace bigtrace
}  // namespace perfetto

int main(int argc, char** argv) {
  auto status = perfetto::bigtrace::WorkerMain(argc, argv);
  if (!status.ok()) {
    fprintf(stderr, "%s\n", status.c_message());
    return 1;
  }
  return 0;
}
