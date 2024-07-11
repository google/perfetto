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
#include "protos/perfetto/bigtrace/worker.grpc.pb.h"
#include "protos/perfetto/bigtrace/worker.pb.h"

#ifndef SRC_BIGTRACE_WORKER_WORKER_IMPL_H_
#define SRC_BIGTRACE_WORKER_WORKER_IMPL_H_

namespace perfetto::bigtrace {

class WorkerImpl final : public protos::BigtraceWorker::Service {
 public:
  grpc::Status QueryTrace(
      grpc::ServerContext*,
      const protos::BigtraceQueryTraceArgs* args,
      protos::BigtraceQueryTraceResponse* response) override;
};

}  // namespace perfetto::bigtrace

#endif  // SRC_BIGTRACE_WORKER_WORKER_IMPL_H_
