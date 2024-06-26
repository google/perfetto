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
#include <mutex>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/status.h>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/waitable_event.h"
#include "protos/perfetto/bigtrace/orchestrator.grpc.pb.h"
#include "protos/perfetto/bigtrace/orchestrator.pb.h"
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"
#include "src/cpp/server/thread_pool_interface.h"

namespace perfetto {
namespace bigtrace {
namespace {

struct CommandLineOptions {
  std::string worker_address;
  uint64_t worker_count;
};

CommandLineOptions ParseCommandLineOptions(int argc, char** argv) {
  CommandLineOptions command_line_options;
  static option long_options[] = {
      {"worker", required_argument, nullptr, 'w'},
      {"num_workers", required_argument, nullptr, 'n'},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "w:n:", long_options, nullptr)) != -1) {
    switch (c) {
      case 'w':
        command_line_options.worker_address = optarg;
        break;
      case 'n':
        command_line_options.worker_count = static_cast<uint64_t>(atoi(optarg));
        break;
      default:
        PERFETTO_ELOG(
            "Usage: %s --worker=worker_address --worker_count=worker_count",
            argv[0]);
        break;
    }
  }

  return command_line_options;
}

class OrchestratorImpl final : public protos::BigtraceOrchestrator::Service {
 public:
  explicit OrchestratorImpl(std::unique_ptr<protos::BigtraceWorker::Stub> stub)
      : stub_(std::move(stub)) {}

 private:
  grpc::Status Query(
      grpc::ServerContext*,
      const protos::BigtraceQueryArgs* args,
      grpc::ServerWriter<protos::BigtraceQueryResponse>* writer) override {
    grpc::Status query_status;
    std::mutex status_lock;
    base::WaitableEvent pool_completion;
    const std::string& sql_query = args->sql_query();
    for (const std::string& trace : args->traces()) {
      pool_.PostTask([&]() {
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
          std::lock_guard<std::mutex> status_guard(status_lock);
          query_status = status;
        } else {
          protos::BigtraceQueryResponse response;
          response.set_trace(trace_response.trace());
          for (const protos::QueryResult& query_result :
               trace_response.result()) {
            response.add_result()->CopyFrom(query_result);
          }
          std::lock_guard<std::mutex> write_guard(write_lock_);
          writer->Write(response);
        }
        pool_completion.Notify();
      });
    }
    pool_completion.Wait(static_cast<uint64_t>(args->traces_size()));
    return query_status;
  }

  base::ThreadPool pool_ = base::ThreadPool(5);
  std::unique_ptr<protos::BigtraceWorker::Stub> stub_;
  std::mutex write_lock_;
};

base::Status OrchestratorMain(int argc, char** argv) {
  CommandLineOptions options = ParseCommandLineOptions(argc, argv);

  std::string server_address("localhost:5051");
  std::string worker_address =
      !options.worker_address.empty() ? options.worker_address : "localhost";

  uint64_t worker_count = options.worker_count;
  PERFETTO_CHECK(worker_count > 0);

  // TODO(ivankc) Replace with DNS resolver
  std::string target_address = "ipv4:";

  for (uint64_t i = 0; i < worker_count; ++i) {
    std::string address = worker_address + ":" + std::to_string(5052 + i) + ",";
    target_address += address;
  }

  grpc::ChannelArguments args;
  args.SetLoadBalancingPolicyName("round_robin");
  auto channel = grpc::CreateCustomChannel(
      target_address, grpc::InsecureChannelCredentials(), args);
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
