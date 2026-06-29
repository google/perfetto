/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/tracing/internal/in_process_tracing_backend.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/tracing_service.h"

#include "src/tracing/core/in_process_shared_memory.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include "src/tracing/service/zlib_compressor.h"
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include "src/tracing/service/zstd_compressor.h"
#endif

// TODO(primiano): When the in-process backend is used, we should never end up
// in a situation where the thread where the TracingService and Producer live
// writes a packet and hence can get into the GetNewChunk() stall.
// This would happen only if the API client code calls Trace() from one of the
// callbacks it receives (e.g. OnStart(), OnStop()). We should either cause a
// hard crash or ignore traces from that thread if that happens, because it
// will deadlock (the Service will never free up the SMB because won't ever get
// to run the task).

namespace perfetto {
namespace internal {

// static
TracingBackend* InProcessTracingBackend::GetInstance() {
  static auto* instance = new InProcessTracingBackend();
  return instance;
}

InProcessTracingBackend::InProcessTracingBackend() = default;
InProcessTracingBackend::~InProcessTracingBackend() = default;

std::unique_ptr<ProducerEndpoint> InProcessTracingBackend::ConnectProducer(
    const ConnectProducerArgs& args) {
  PERFETTO_DCHECK(args.task_runner->RunsTasksOnCurrentThread());
  return GetOrCreateService(args.task_runner)
      ->ConnectProducer(args.producer,
                        ClientIdentity(/*uid=*/0, /*pid=*/0, args.machine_id),
                        args.producer_name, args.shmem_size_hint_bytes,
                        /*in_process=*/true,
                        TracingService::ProducerSMBScrapingMode::kEnabled,
                        args.shmem_page_size_hint_bytes);
}

std::unique_ptr<ConsumerEndpoint> InProcessTracingBackend::ConnectConsumer(
    const ConnectConsumerArgs& args) {
  return GetOrCreateService(args.task_runner)
      ->ConnectConsumer(args.consumer, /*uid=*/0);
}

TracingService* InProcessTracingBackend::GetOrCreateService(
    base::TaskRunner* task_runner) {
  if (!service_) {
    std::unique_ptr<InProcessSharedMemory::Factory> shm(
        new InProcessSharedMemory::Factory());
    TracingService::InitOpts init_opts = {};
    // Wire the compressors so TraceConfig compression takes effect on the
    // in-process backend's service, mirroring src/traced/service/service.cc.
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
    init_opts.deflate_compressor_fn = &ZlibCompressFn;
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
    init_opts.zstd_compressor_fn = &ZstdCompressFn;
#endif
    service_ =
        TracingService::CreateInstance(std::move(shm), task_runner, init_opts);
    service_->SetSMBScrapingEnabled(true);
  }
  return service_.get();
}

}  // namespace internal
}  // namespace perfetto
