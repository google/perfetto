/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_
#define SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_

#include <functional>
#include <memory>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/tracing_service.h"

namespace perfetto {

namespace tracing_v2 {
class InProcessTracingV2Bridge;
}

namespace internal {

// True when the v2 data path should be used. Off by default: gated on the
// Android aconfig flag, and forced off entirely on platforms without a futex.
// The aconfig flag grants global feature permission; it does not identify the
// process or backend selected for rollout. Tests use the override, which cannot
// defeat the platform gate and must be set before Tracing::Initialize() while
// no tracing threads exist, then reset afterwards.
bool UseTracingV2InProcess();
void SetTracingV2InProcessForTesting(bool);

// Producer-side decorator used by the SDK migration. Every CreateTraceWriter()
// call, whatever its exhaustion policy, gets a writer backed by the v2
// in-process ring, whose packets are relayed back through an ordinary writer
// from |endpoint|. Startup writers go via MaybeSharedMemoryArbiter() and so
// never reach this. This whole decorator is migration scaffolding; delete it
// when traced maps and consumes the v2 ring directly.
//
// Three method contracts:
//   - CreateTraceWriter() is thread-safe and runs on arbitrary SDK threads.
//   - Registration and the other control-plane calls are muxer-sequence
//     passthroughs.
//   - NotifyFlushComplete(), NotifyDataSourceStopped() and Sync() are
//     asynchronous ring-watermark fences: they return to the muxer immediately
//     and reach |endpoint| later, once the relay has caught up.
//
// |relay_task_runner| is the muxer-owned sequence that drains the ring. It is
// deliberately not the muxer's own: the relay stands in for the traced-side
// consumer, and control-plane congestion must not stop the ring being read.
class TracingV2ProducerEndpoint : public ProducerEndpoint {
 public:
  TracingV2ProducerEndpoint(std::unique_ptr<ProducerEndpoint>,
                            base::TaskRunner* muxer_task_runner,
                            base::TaskRunner* relay_task_runner);
  ~TracingV2ProducerEndpoint() override;

  TracingV2ProducerEndpoint(const TracingV2ProducerEndpoint&) = delete;
  TracingV2ProducerEndpoint& operator=(const TracingV2ProducerEndpoint&) =
      delete;

  void Disconnect() override;
  void RegisterDataSource(const DataSourceDescriptor&) override;
  void UpdateDataSource(const DataSourceDescriptor&) override;
  void UnregisterDataSource(const std::string&) override;
  void RegisterTraceWriter(uint32_t writer_id, uint32_t target_buffer) override;
  void UnregisterTraceWriter(uint32_t writer_id) override;
  void CommitData(const CommitDataRequest&,
                  CommitDataCallback callback = {}) override;
  SharedMemory* shared_memory() const override;
  size_t shared_buffer_page_size_kb() const override;
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy) override;
  SharedMemoryArbiter* MaybeSharedMemoryArbiter() override;
  bool IsShmemProvidedByProducer() const override;
  void NotifyFlushComplete(FlushRequestID) override;
  void NotifyDataSourceStarted(DataSourceInstanceID) override;
  void NotifyDataSourceStopped(DataSourceInstanceID) override;
  void ActivateTriggers(const std::vector<std::string>&) override;
  void Sync(std::function<void()> callback) override;

 private:
  // Receives a validated |this| once the fence completes, so a continuation
  // never captures a raw endpoint pointer it might outlive.
  using RelayContinuation = std::function<void(TracingV2ProducerEndpoint*)>;

  // Fences a control operation against the ring: samples the ring position
  // now, and runs |continuation| back on the muxer sequence once the relay has
  // forwarded everything published before it. Returns immediately - the muxer
  // must stay runnable, because the relay can need it to issue the arbiter's
  // pending commits before it can make progress.
  void RunAfterRelayDrained(RelayContinuation continuation);

  // Pushes the commits the relay issued from its own thread, which the arbiter
  // may still be batching. The muxer sequence is the only one allowed to do
  // this; see SharedMemoryArbiterImpl::GetNewChunk().
  void FlushPendingCommitDataRequests();

  std::unique_ptr<ProducerEndpoint> endpoint_;
  base::TaskRunner* const muxer_task_runner_;
  std::shared_ptr<tracing_v2::InProcessTracingV2Bridge> bridge_;

  // Last member: a pending fence must find this invalid once the endpoint is
  // gone. Created and dereferenced only on the muxer sequence; the relay may
  // copy a weak pointer but never touch it.
  base::WeakPtrFactory<TracingV2ProducerEndpoint> weak_ptr_factory_{this};
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_
