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

#ifndef SRC_TRACING_SERVICE_RING_BUFFER_INGRESS_H_
#define SRC_TRACING_SERVICE_RING_BUFFER_INGRESS_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_runner.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"

namespace perfetto {
class TraceBufferV2;
namespace tracing_v2 {

// Drains one producer's tracing v2 RingBuffer into authorized trace buffers.
// ProducerEndpointImpl owns this ingress and implements its Delegate.
// All calls, reader callbacks, and destruction use the service sequence.
//
// The ingress owns the mapping, the SharedRingBuffer view, and the reader.
// The endpoint keeps the delegate alive until this ingress is destroyed.
// The caller must keep the borrowed task runner alive until endpoint
// destruction.
//
// Destruction cancels retry tasks before it destroys the reader, view, and
// mapping, in that order. The ingress never owns a destination trace buffer.
class RingBufferIngress : public SharedRingBufferReader::Delegate {
 public:
  // Resolves authorized destinations and records discarded chunks.
  // The ingress calls this interface on the service sequence. Calls must not
  // destroy or reenter the ingress. The delegate must outlive the ingress.
  class Delegate {
   public:
    virtual ~Delegate();

    // Returns an authorized, eligible trace buffer, or nullptr.
    // The service owns the result. The ingress borrows it for the current call.
    virtual TraceBufferV2* GetRingBufferDestination(BufferID) = 0;

    // Calls |callback| inline for each authorized, eligible trace buffer.
    // Each reference borrows service-owned storage for that callback only.
    // The callback must not change the set of destinations or destroy a buffer.
    virtual void ForEachRingBufferDestination(
        const std::function<void(TraceBufferV2&)>& callback) = 0;

    // Increments the service statistic for a discarded RingBuffer chunk.
    // This also counts loss before a trace buffer admits the writer's first
    // chunk. Such loss has no destination sequence to update.
    virtual void OnRingBufferChunkDiscarded() = 0;

    // Reports the first admitted chunk into |buffer_id|. The ingress calls
    // this once per destination for observability. The delegate can mark
    // sessions that own the buffer for a fresh stats snapshot.
    virtual void OnRingBufferUsed(BufferID buffer_id) = 0;
  };

  // The endpoint transfers a validated mapping on the service sequence.
  // The view borrows |memory|. The reader borrows the view and this ingress.
  // |delegate| and |task_runner| remain borrowed for the ingress lifetime.
  RingBufferIngress(std::unique_ptr<SharedMemory> memory,
                    uint32_t chunk_size_bytes,
                    ProducerID,
                    ClientIdentity,
                    Delegate* delegate,
                    base::TaskRunner* task_runner);

  // The endpoint destroys this ingress on the service sequence.
  // Retry tasks cannot access it after destruction starts.
  ~RingBufferIngress() override;
  RingBufferIngress(const RingBufferIngress&) = delete;
  RingBufferIngress& operator=(const RingBufferIngress&) = delete;
  RingBufferIngress(RingBufferIngress&&) = delete;
  RingBufferIngress& operator=(RingBufferIngress&&) = delete;

  // The endpoint requests one pass on the service sequence, without a reply.
  // The pass covers reservations up to the write position sampled at entry.
  // The reader consumes published fragments and skips unpublished reservations.
  //
  // If a writer wins repeated state transitions, one posted task retries the
  // drain. Each retry samples the write position again.
  //
  // A reader protocol error stops this and all later passes. The ingress keeps
  // the mapping until destruction. A pending retry becomes a no-op.
  void Drain();

  // The service reads the owned mapping size for its memory guardrail.
  // The call uses the service sequence and transfers no ownership.
  size_t size_bytes() const { return memory_->size(); }

  // Returns a snapshot of connection-wide layout, reader counters, and the
  // subset of |session_buffers| this ingress has already used.
  // Returns nullopt if none of |session_buffers| have been used.
  std::optional<TraceStats::V2ProducerStats> GetStats(
      const std::vector<BufferID>& session_buffers) const;

  // Removes |id| from the observed set. The service calls this when the
  // matching trace buffer goes away so that a later reuse of the id counts
  // as first use again.
  void ForgetBuffer(BufferID id) {
    PERFETTO_DCHECK_THREAD(thread_checker_);
    observed_buffers_.erase(id);
  }

 private:
  // Reader callbacks run inline during Drain() on the service sequence.
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Trusted producer ID supplied by the endpoint for each sequence key.
  const ProducerID producer_id_;
  // Trusted client identity supplied by the endpoint for appended fragments.
  const ClientIdentity client_identity_;

  // Borrowed endpoint delegate. The endpoint destroys this ingress first.
  Delegate* const delegate_;

  // Sole owner of the mapping. Declared before both objects that borrow it.
  std::unique_ptr<SharedMemory> memory_;

  // Validated view of the mapping. The view does not initialize shared bytes.
  SharedRingBuffer ring_buffer_;

  // Sole reader. Its callbacks borrow this ingress and its scratch payloads.
  SharedRingBufferReader reader_;

  // True while one retry task is pending. Further drains do not post
  // duplicates.
  bool retry_scheduled_ = false;

  // Trace buffers that this ingress has admitted at least one chunk into.
  // Populated once per destination on the first successful append.
  std::set<BufferID> observed_buffers_;

  // Verifies that operations use the service sequence.
  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Declared last to cancel retry tasks before reader or mapping destruction.
  base::WeakRunner weak_runner_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_RING_BUFFER_INGRESS_H_
