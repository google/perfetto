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

#include "perfetto/ext/base/threading/thread_pool.h"
#include "protos/perfetto/bigtrace/orchestrator.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"

#ifndef SRC_BIGTRACE_ORCHESTRATOR_ORCHESTRATOR_IMPL_H_
#define SRC_BIGTRACE_ORCHESTRATOR_ORCHESTRATOR_IMPL_H_

namespace perfetto {
namespace bigtrace {
class OrchestratorImpl final : public protos::BigtraceOrchestrator::Service {
 public:
  explicit OrchestratorImpl(std::unique_ptr<protos::BigtraceWorker::Stub> stub);
  grpc::Status Query(
      grpc::ServerContext*,
      const protos::BigtraceQueryArgs* args,
      grpc::ServerWriter<protos::BigtraceQueryResponse>* writer) override;

 private:
  std::unique_ptr<protos::BigtraceWorker::Stub> stub_;
  base::ThreadPool pool_;
  std::mutex write_lock_;
};
}  // namespace bigtrace
}  // namespace perfetto

#endif  // SRC_BIGTRACE_ORCHESTRATOR_ORCHESTRATOR_IMPL_H_
