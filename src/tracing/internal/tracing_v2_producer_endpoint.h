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
#include <string>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/tracing/core/tracing_service.h"

namespace perfetto {

namespace tracing_v2 {
class InProcessTracingV2Bridge;
}

namespace internal {

// True when the v2 data path should be used. Off by default: gated on the
// Android aconfig flag, and forced off entirely on platforms without a futex,
// because a writer that fills the ring has nothing to wait on there. The
// aconfig flag grants global feature permission; it does not identify the
// process or backend selected for rollout.
//
// Tests use the override, which cannot defeat the platform gate. It must be set
// before Tracing::Initialize(), while no tracing thread exists, and reset
// afterwards.
bool UseTracingV2InProcess();
void SetTracingV2InProcessForTesting(bool);

// How many TraceWriters have been routed onto a v2 ring since process start.
// Test-only: an enabled and a disabled run produce the same trace, so this is
// what lets a test assert that the gate actually took effect rather than
// inferring it from output that would look identical either way.
uint64_t GetTracingV2WritersCreatedForTesting();

// Producer-side decorator used by the SDK migration. Every CreateTraceWriter()
// call, whatever its exhaustion policy, gets a writer backed by the v2
// in-process ring, whose packets are relayed back through an ordinary writer
// from |endpoint|. Startup writers are created directly by the arbiter, via
// MaybeSharedMemoryArbiter(), and so never reach this. The whole decorator is
// migration scaffolding: delete it when traced maps and consumes the v2 ring.
//
// Method contracts:
//   - CreateTraceWriter() is thread-safe and runs on arbitrary SDK threads.
//   - Everything else is a muxer-sequence passthrough.
//
// TODO(sashwinbalaji): a control-plane call is a plain forward, so it can
// overtake trace data still sitting in the ring. NotifyFlushComplete(),
// NotifyDataSourceStopped() and Sync() all promise the service that everything
// this producer sent before them has arrived; with a v1 writer that is true by
// construction, because the bytes are already in the shared memory buffer, but
// a v2 packet is only in the ring until the relay moves it. Ordering the two
// properly needs the producer-to-service data-plane protocol to queue producer
// notifications behind the SMB read - it is not something this scaffolding can
// fake, and the alternative (fencing every control call on a ring position)
// was tried and is exactly the watermark machinery that Step 1 removed. The
// path is default-off until then. Tests that need the ordering use
// WaitForRelayQuiescenceForTesting().
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
  TracingV2ProducerEndpoint(TracingV2ProducerEndpoint&&) = delete;
  TracingV2ProducerEndpoint& operator=(TracingV2ProducerEndpoint&&) = delete;

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

  // Blocks until everything published to the v2 ring before this call has been
  // forwarded through the temporary v1 hop and acknowledged by the service.
  //
  // Test-only, and deliberately not the production flush protocol: see the
  // ordering TODO above. Requires live muxer and relay threads and must not be
  // called on either; the caller must keep this endpoint alive for the
  // duration.
  void WaitForRelayQuiescenceForTesting();

  // Starts making the relay let go of the downstream v1 writers it borrowed
  // from this endpoint. Returns immediately, and is idempotent.
  //
  // The muxer calls this when it disposes the connection, and again before it
  // tears itself down. It has to: those writers hold WriterIDs in this
  // endpoint's arbiter, and while they do the arbiter cannot shut down, so the
  // muxer would keep the whole dead backend - and therefore this endpoint, and
  // therefore the bridge, and therefore the writers - alive forever.
  //
  // It must not wait. A drain already running on the relay can be inside a
  // downstream writer waiting for this endpoint's arbiter to be pumped, which
  // only the muxer sequence may do, so a muxer that waited here would be
  // waiting for itself. The caller keeps this endpoint alive until
  // relay_writers_released() is true instead; the arbiter does that on its own,
  // because a retained writer holds a WriterID and an arbiter with outstanding
  // WriterIDs refuses to shut down.
  void StartReleasingRelayWriters();

  // Whether the relay is still holding downstream writers borrowed from this
  // endpoint. While it is, the arbiter they belong to has outstanding WriterIDs
  // and refuses to shut down, which is what keeps this endpoint alive.
  bool relay_retains_writers() const;

 private:
  // Pushes the commits the relay issued from its own thread, which the arbiter
  // may still be batching. Only the muxer sequence is allowed to do this; see
  // SharedMemoryArbiterImpl::GetNewChunk().
  void FlushPendingCommitDataRequests();

  // Destroyed after the bridge has released the downstream writers that depend
  // on its arbiter; see the destructor.
  const std::unique_ptr<ProducerEndpoint> endpoint_;
  base::TaskRunner* const muxer_task_runner_;
  // Null when the ring could not be allocated, in which case this decorator
  // forwards everything and creates plain v1 writers.
  const std::shared_ptr<tracing_v2::InProcessTracingV2Bridge> bridge_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_
