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

#include <grpcpp/grpcpp.h>

#include "perfetto/base/status.h"

namespace perfetto {
namespace bigtrace {
namespace {

base::Status OrchestratorMain(int, char**) {
  std::string server_address("127.0.0.1:5051");
  grpc::ServerBuilder builder;
  auto cq = builder.AddCompletionQueue();
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

  auto channel =
      grpc::CreateChannel("localhost:5052", grpc::InsecureChannelCredentials());
  bool connected = channel->WaitForConnected(std::chrono::system_clock::now() +
                                             std::chrono::milliseconds(5000));

  PERFETTO_CHECK(connected);

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
