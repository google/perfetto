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

#ifndef SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_DRAINER_H_
#define SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_DRAINER_H_

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
class ServiceRingBufferDrainerTestPeer;
}  // namespace test

// Copies published data from one producer connection's v2 ring buffer into
// authorized TraceBufferV2 destinations. ProducerRingBufferArbiter manages the
// producer side. See AttachV2RingBuffer in producer_port.proto for the IPC
// flow.
//
// All operations, including construction and destruction, run on the service
// sequence.
//
//   ProducerEndpointImpl
//     |-- owns ---> ServiceRingBufferDrainer (this class)
//                     |-- shares -> SharedMemory: the ring buffer mapping
//                     |-- owns ---> SharedRingBuffer: the view
//                     |-- owns ---> SharedRingBufferReader
//                     |-- uses ---> Delegate (the ProducerEndpointImpl)
//                     |-- uses ---> the service task runner
//
// Over IPC, |memory_| holds the service's separate mapping. In-process, the
// producer and service share the same mapping.
class ServiceRingBufferDrainer : public SharedRingBufferReader::Delegate {
 public:
  // Delegate calls must not destroy or reenter the drainer.
  class Delegate {
   public:
    virtual ~Delegate();

    // Returns a TraceBufferV2 that the producer may write to, or nullptr.
    // The service owns the buffer, which must remain valid during
    // OnChunkRead().
    virtual TraceBufferV2* GetRingBufferDestination(BufferID) = 0;

    // Calls |callback| inline for each buffer that GetRingBufferDestination()
    // would return. Buffer references are valid only during the callback.
    // The callback must not change the set of destinations or destroy a buffer.
    virtual void ForEachRingBufferDestination(
        const std::function<void(TraceBufferV2&)>& callback) = 0;

    // Adds |count| to the service's chunks_discarded statistic for chunks that
    // never reached a trace buffer. Excludes loss reported by the producer.
    virtual void OnRingBufferChunksDiscarded(uint64_t count) = 0;
  };

  // Shares ownership of |memory|. The mapping and |chunk_size_bytes| must
  // describe a valid ring buffer layout. |delegate| and |task_runner| must
  // remain valid until destruction.
  ServiceRingBufferDrainer(std::shared_ptr<SharedMemory> memory,
                           uint32_t chunk_size_bytes,
                           ProducerID,
                           ClientIdentity,
                           Delegate* delegate,
                           base::TaskRunner* task_runner);

  // Cancels pending retry tasks. Destruction does not call Drain() or copy any
  // remaining data from |ring_buffer_|.
  ~ServiceRingBufferDrainer() override;
  ServiceRingBufferDrainer(const ServiceRingBufferDrainer&) = delete;
  ServiceRingBufferDrainer& operator=(const ServiceRingBufferDrainer&) = delete;
  ServiceRingBufferDrainer(ServiceRingBufferDrainer&&) = delete;
  ServiceRingBufferDrainer& operator=(ServiceRingBufferDrainer&&) = delete;

  // Reads at most |ring_buffer_.num_chunks()| logical positions. Reaching the
  // position or retry limit schedules another pass after a delay. At most one
  // retry task is pending at a time.
  //
  // A protocol error stops all future reads and records one ABI violation in
  // each authorized destination. Data already copied is kept. The producer
  // stays connected without notification or a v1 fallback. When the ring buffer
  // fills, its writers drop packets, or stall and time out.
  void Drain();

  // Mapping size for the service's memory guardrail.
  size_t size_bytes() const { return memory_->size(); }

  // The layout of the ring buffer, for TraceStats.
  uint32_t chunk_size() const { return ring_buffer_.chunk_size(); }
  uint32_t num_chunks() const { return ring_buffer_.num_chunks(); }

 private:
  friend class test::ServiceRingBufferDrainerTestPeer;

  // SharedRingBufferReader::Delegate implementation.
  // The reader calls these inline during Drain().
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Marks a gap in the sequence for |writer_id| in its destination buffer.
  void RecordWriterLoss(WriterID writer_id);

  // Trusted identity supplied by the endpoint.
  const ProducerID producer_id_;
  const ClientIdentity client_identity_;

  Delegate* const delegate_;

  // Declared before |ring_buffer_| and |reader_| so the mapping outlives them.
  std::shared_ptr<SharedMemory> memory_;

  // Constructing |ring_buffer_| leaves the contents of |memory_| unchanged,
  // preserving any data the producer has already written.
  SharedRingBuffer ring_buffer_;
  SharedRingBufferReader reader_;

  bool retry_scheduled_ = false;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Declared last to cancel retry tasks before reader or mapping destruction.
  base::WeakRunner weak_runner_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_DRAINER_H_
