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

#ifndef SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_ENDPOINT_H_
#define SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_ENDPOINT_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>

#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_runner.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"

namespace perfetto {
class TraceBufferV2;
namespace tracing_v2 {

namespace test {
class ServiceRingBufferEndpointTestPeer;
}  // namespace test

// The service side of one producer's tracing v2 ring buffer. It drains the
// ring buffer into authorized trace buffers.
//
// It is not a trace buffer and never owns one. The producer side of the same
// ring buffer is ProducerRingBufferEndpoint. For the IPC protocol between the
// two sides, see rpc ShareRingBuffer in producer_port.proto.
//
// Two processes map the same memfd:
//
//   producer process                   traced
//   --------------------------------   --------------------------------
//   ProducerRingBufferEndpoint         ServiceRingBufferEndpoint (this)
//     owns the producer's mapping        owns the service's mapping
//     writers publish chunks             one reader drains the chunks
//
// Both mappings show the same pages. Each side unmaps only its own mapping.
//
// Threads:
//
// All calls, reader callbacks and destruction use the service sequence.
//
// Ownership:
//
// "owns" is a unique_ptr or a member. "uses" is a raw pointer.
//
//   ProducerEndpointImpl
//     |-- owns --> ServiceRingBufferEndpoint (this class)
//                    |-- owns --> SharedMemory: the service's mapping
//                    |-- owns --> SharedRingBuffer: the view
//                    |-- owns --> SharedRingBufferReader
//                    |-- uses --> Delegate (the ProducerEndpointImpl)
//                    |-- uses --> the service task runner
//
// - The owner keeps the delegate and the task runner alive until this object
//   is destroyed.
// - The last drain runs before destruction, in
//   TracingServiceImpl::DisconnectProducer(). The destructor does not drain,
//   because its owner, the delegate, is being destroyed at that point.
// - Destruction cancels retry tasks, then destroys the reader, the view and
//   the mapping, in that order.
class ServiceRingBufferEndpoint : public SharedRingBufferReader::Delegate {
 public:
  // Resolves authorized destinations and records discarded chunks.
  // This object calls the delegate on the service sequence. Calls must not
  // destroy or reenter this object. The delegate must outlive it.
  class Delegate {
   public:
    virtual ~Delegate();

    // Returns the buffer if the producer may write to it and it is a TBv2
    // buffer. Otherwise returns nullptr.
    // The service owns the result. This object borrows it for the current
    // call.
    virtual TraceBufferV2* GetRingBufferDestination(BufferID) = 0;

    // Calls |callback| inline for each buffer that GetRingBufferDestination()
    // would return.
    // Each reference borrows service-owned storage for that callback only.
    // The callback must not change the set of destinations or destroy a buffer.
    virtual void ForEachRingBufferDestination(
        const std::function<void(TraceBufferV2&)>& callback) = 0;

    // Adds |count| to the service statistic chunks_discarded. It counts
    // chunks that the service drops before any trace buffer sees them:
    // - an invalid writer ID or an unauthorized destination,
    // - a chunk that the reader finds malformed or in an unknown format.
    // Loss that the producer flagged is not counted here.
    virtual void OnRingBufferChunksDiscarded(uint64_t count) = 0;
  };

  // The endpoint transfers a validated mapping on the service sequence.
  // The view borrows |memory|. The reader borrows the view and this object.
  // |delegate| and |task_runner| remain borrowed for the lifetime of this
  // object.
  ServiceRingBufferEndpoint(std::unique_ptr<SharedMemory> memory,
                            uint32_t chunk_size_bytes,
                            ProducerID,
                            ClientIdentity,
                            Delegate* delegate,
                            base::TaskRunner* task_runner);

  // The endpoint destroys this object on the service sequence.
  // Retry tasks cannot access it after destruction starts.
  ~ServiceRingBufferEndpoint() override;
  ServiceRingBufferEndpoint(const ServiceRingBufferEndpoint&) = delete;
  ServiceRingBufferEndpoint& operator=(const ServiceRingBufferEndpoint&) =
      delete;
  ServiceRingBufferEndpoint(ServiceRingBufferEndpoint&&) = delete;
  ServiceRingBufferEndpoint& operator=(ServiceRingBufferEndpoint&&) = delete;

  // The endpoint requests one pass on the service sequence, without a reply.
  // - The pass consumes at most one position per chunk.
  // - The reader consumes published fragments and skips unpublished
  //   reservations.
  // - If the pass stops early, one delayed task retries the drain. Examples:
  //   a writer wins repeated state transitions, or writers reserve more
  //   positions during the pass.
  //
  // A reader protocol error stops the ring buffer for good:
  // - This and all later passes do nothing. A pending retry does nothing.
  // - Each destination buffer at that time counts one ABI violation.
  // - This object keeps the mapping until destruction.
  // - The producer is not told and stays connected. There is no v1
  //   fallback. Its ring buffer writers drop packets, or stall and time out,
  //   when the ring buffer is full.
  void Drain();

  // The service reads the owned mapping size for its memory guardrail.
  // The call uses the service sequence and transfers no ownership.
  size_t size_bytes() const { return memory_->size(); }

 private:
  friend class test::ServiceRingBufferEndpointTestPeer;

  // SharedRingBufferReader::Delegate:
  // The reader calls these inline during Drain() on the service sequence.
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Marks a gap on the sequence of |writer_id|, in whichever destination
  // holds it.
  void RecordWriterLoss(WriterID writer_id);

  // Trusted producer ID supplied by the endpoint for each sequence key.
  const ProducerID producer_id_;
  // Trusted client identity supplied by the endpoint for appended fragments.
  const ClientIdentity client_identity_;

  // Borrowed endpoint delegate. The endpoint destroys this object first.
  Delegate* const delegate_;

  // Sole owner of the service's mapping of the memfd. The producer owns its
  // own mapping of the same memfd. Declared before both objects that borrow
  // it.
  std::unique_ptr<SharedMemory> memory_;

  // Validated view of the mapping. The view does not initialize shared bytes.
  SharedRingBuffer ring_buffer_;

  // Sole reader. Its callbacks borrow this object and its scratch payloads.
  SharedRingBufferReader reader_;

  // True while one retry task is pending. Further drains do not post
  // duplicates.
  bool retry_scheduled_ = false;

  // Verifies that operations use the service sequence.
  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Declared last to cancel retry tasks before reader or mapping destruction.
  base::WeakRunner weak_runner_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_ENDPOINT_H_
