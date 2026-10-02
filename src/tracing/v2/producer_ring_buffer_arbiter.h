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

#ifndef SRC_TRACING_V2_PRODUCER_RING_BUFFER_ARBITER_H_
#define SRC_TRACING_V2_PRODUCER_RING_BUFFER_ARBITER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/rt_mutex.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class ProducerEndpoint;
class SharedMemory;
class SharedMemoryArbiter;
class TraceWriter;

namespace tracing_v2 {

namespace test {
class ProducerRingBufferArbiterTestPeer;
}  // namespace test

// The producer side of one tracing v2 ring buffer.
//
// The producer's ProducerEndpoint (for example ProducerIPCClientImpl) creates
// one arbiter for each ring buffer that it shares with the service. In this
// file:
// - "endpoint" means that ProducerEndpoint.
// - "SMB arbiter" means the SharedMemoryArbiter used for v1 tracing.
// - "writer" means TraceWriterV2Impl.
//
// This class:
// - Selects v1 or v2 for each data source instance:
//   - Writers created with the instance ID get that choice.
//   - Writers created without it, such as startup writers or
//     CreateTraceWriter(buffer, policy), are v1 writers.
// - Creates one ring buffer for the whole connection, when the first instance
//   selects v2, and attaches it to the service.
// - Keeps the ring buffer mapping alive and owns the SharedRingBuffer view.
// - Creates writers. Their WriterIDs come from the SMB arbiter, because v1
//   and v2 writers of one producer share one ID pool.
// - Tracks the service reader. See ReaderState.
// - Delivers drain requests to the endpoint thread, which sends
//   DrainV2RingBuffer to the service. See DrainUrgency.
// - Runs each Flush() callback after its drain request.
//
// Threads:
// - Writers call ring_buffer(), RequestDrain(), IsReaderAttached(), Flush()
//   and OnWriterDestroyed() from any thread. MaybeCreateTraceWriter() can also
//   run on any thread.
// - Creation, state changes, posted tasks, flush callbacks and service
//   requests run on the endpoint thread.
//
// Flush(callback) posts its own drain task, then the callback. Time goes
// down. "===>" is a posted task, or an IPC request to the service.
//
//     writer thread           endpoint thread         service
//        |                       |                       |
//        | Flush(callback)       |                       |
//        |==== drain task ======>|                       |
//        |==== callback ========>|                       |
//        |                       |== DrainV2RingBuffer =>| copies chunks
//        |                       | callback()            |
//
// A message that the callback sends reaches the service after the drain
// request. The service handles them in order.
//
// The endpoints do not ask for a drain before their flush and stop acks.
// The service drains the ring buffer by itself after the last flush ack,
// after the last stop ack, and when the producer disconnects
// (TracingServiceImpl::ScrapeSharedMemoryBuffers()).
//
// The first data source instance that selects v2 creates the ring buffer.
// Time goes down.
// "--->" is a direct call, and "===>" is an IPC message.
//
// endpoint                   this class                   service
//    |                           |                           |
//    | SetupInstance()           |                           |
//    |-------------------------->| allocates the memory      |
//    |                           | kNoRingBuffer -> kPending |
//    |<-- AttachV2RingBuffer() --|                           |
//    |== AttachV2RingBuffer, with the memfd ================>| maps, checks,
//    |<== reply =============================================| attaches
//    |-- reply callback -------->| kAttached, or             |
//    |                           | kDetached if rejected     |
//
// - Later instances reuse the ring buffer.
// - If the ring buffer cannot be created, the state moves to kDetached, and
//   later instances do not try again.
//
// In-process, the endpoint is the service's own ProducerEndpointImpl:
// - Its AttachV2RingBuffer() and DrainV2RingBuffer() are direct calls, not
//   IPC messages.
// - The service uses the same mapping.
// - The attach reply runs before AttachV2RingBuffer() returns.
//
// Ownership:
//
// - "owns": the owner destroys it, as a member or through a unique_ptr.
// - "shares": a shared_ptr keeps it alive while any owner holds it.
// - "uses": a raw pointer. The object must outlive the user.
// - "holds": an allocated ID. The holder must release it.
//
//   endpoint (for example ProducerIPCClientImpl)
//     |-- owns --> SharedMemoryArbiterImpl: the WriterID pool
//     |-- owns --> ProducerRingBufferArbiter (this class)
//                    |-- shares-> SharedMemory: the ring buffer mapping
//                    |-- owns --> SharedRingBuffer: the view
//                    |-- uses --> endpoint, SharedMemoryArbiterImpl
//
//   data source (SDK)
//     |-- owns --> TraceWriterV2Impl
//                    |-- uses --> ProducerRingBufferArbiter
//                    |-- holds -> a WriterID from SharedMemoryArbiterImpl
//                    |-- owns --> SharedRingBufferWriter
//                                   |-- uses --> SharedRingBuffer
//
// Writers use raw pointers into objects that the endpoint owns. The WriterID
// keeps these pointers valid:
// - While a writer holds a WriterID, TryShutdown() of the SMB arbiter fails.
// - So the endpoint keeps the SMB arbiter, this object and the mapping.
// - The writer releases its WriterID last, in OnWriterDestroyed().
class ProducerRingBufferArbiter {
 public:
  // Allocates |size| bytes for the v2 ring buffer.
  // On failure, it logs the reason and returns null.
  // The endpoint provides it, because the memory depends on the transport: a
  // sealed memfd over IPC, or plain memory in-process.
  using AllocateV2RingBufferFn =
      std::function<std::shared_ptr<SharedMemory>(size_t size)>;

  // Whether writers can rely on a service reader to free space.
  // - Only the endpoint thread changes the state, but writers can read it
  //   from any thread.
  // - This class sends drain requests in every state. The service handles
  //   attach and drain requests in order. It ignores a drain for a ring
  //   buffer that it did not accept.
  //
  //                   SetupInstance()            OnReaderAttached()
  //   [kNoRingBuffer] --------------> [kPending] -----------------> [kAttached]
  //          |                             |                             |
  //          +-----------------------------+-----------------------------+
  //                                        | Disconnect()
  //                                        v
  //                                   [kDetached]
  //                                   (terminal)
  enum class ReaderState {
    // No instance selected v2 yet, so there is no ring buffer and no writer.
    // - Tracing v2 is still an experiment that few connections use, so the
    //   ring buffer is created only when the first instance selects v2.
    kNoRingBuffer,

    // No reader yet. The endpoint shared the ring buffer and waits for the
    // service reply.
    // - Writers can already publish.
    // - The service can still reject the ring buffer.
    // - A writer does not wait for space in a full ring buffer. It drops the
    //   packet instead, also under kStall.
    kPending,

    // The service accepted the ring buffer. Its reader drains it.
    // - A writer can wait for space in a full ring buffer.
    kAttached,

    // Terminal. No reader now or later. Disconnect() enters it after
    // rejection, disconnect or destruction of this class.
    // - New writers are NullTraceWriters.
    // - Existing writers keep the mapping. They drop packets when the ring
    //   buffer is full.
    kDetached,
  };

  // How soon RequestDrain() must send the drain request.
  //
  // Only the endpoint thread sends requests. Normally RequestDrain() posts a
  // task there, and all requests made before the task runs share it. This
  // saves IPC traffic. But if the caller is on the endpoint thread and blocks
  // it right after RequestDrain(), the task cannot run until the caller
  // unblocks, and the caller waits for that drain. The urgency tells
  // RequestDrain() whether the request can wait for a task.
  //
  // It changes only when the request is sent. The service handles every
  // request the same way, and RequestDrain() never waits for the drain.
  enum class DrainUrgency {
    // Set it when the caller does not block after RequestDrain().
    // A task on the endpoint thread sends the request. Requests made before
    // that task runs share it, so a burst of calls costs one request.
    kRoutine,

    // Set it when the caller blocks right after RequestDrain(), until the
    // service reads the ring buffer.
    // - On the endpoint thread, sends the request at once, before
    //   RequestDrain() returns.
    // - On other threads, the same as kRoutine. The endpoint thread is free
    //   to run the task.
    kUrgent,
  };

  // |endpoint| receives AttachV2RingBuffer() and the drain requests.
  // |task_runner| runs the endpoint thread.
  // Both must outlive this object.
  ProducerRingBufferArbiter(base::TaskRunner* task_runner,
                            ProducerEndpoint* endpoint,
                            AllocateV2RingBufferFn allocate_v2_ring_buffer);

  // The endpoint destroys this on its thread after all writers release their
  // IDs. Calls Disconnect().
  ~ProducerRingBufferArbiter();

  // Writers keep a pointer to this object.
  ProducerRingBufferArbiter(const ProducerRingBufferArbiter&) = delete;
  ProducerRingBufferArbiter& operator=(const ProducerRingBufferArbiter&) =
      delete;

  // Data source instances:

  // Selects v1 or v2 for a new data source instance.
  // Call it at setup, before the producer sees the instance.
  // - v2 needs v2 in |protocol_abi_versions|, supports_tracing_v2 in the
  //   config, and a hit of the probability sample.
  // - The first v2 instance creates and attaches the ring buffer, with at most
  //   |size_budget| bytes of chunks (a default size if zero).
  void SetupInstance(DataSourceInstanceID,
                     const DataSourceConfig&,
                     uint32_t protocol_abi_versions,
                     size_t size_budget);

  // Forgets the instance, after the producer reports that it stopped.
  // A data source that stops asynchronously can still create writers until
  // then, and they keep the instance's transport.
  void OnInstanceStopped(DataSourceInstanceID);

  // Reader state:

  // True in kAttached only. If false, nothing frees space in a full ring
  // buffer. Writers call it on their own thread.
  bool IsReaderAttached() const;

  // The endpoint calls this on its thread after rejection or disconnect.
  // Enters kDetached. Repeated calls are safe.
  void Disconnect();

  // Writers:

  // Creates a writer for |target_buffer|, on any thread. The caller owns the
  // writer.
  //
  // Returns nullptr if the instance did not select v2, so that the endpoint
  // can create a v1 writer instead.
  // For a v2 instance it never returns nullptr, but returns a NullTraceWriter,
  // which discards all packets, if:
  // - the state is kDetached, or
  // - the SMB arbiter has no free WriterID (exhaustion or shutdown).
  std::unique_ptr<TraceWriter> MaybeCreateTraceWriter(BufferID,
                                                      BufferExhaustedPolicy,
                                                      DataSourceInstanceID);

  // The writer calls this on its thread after its final publication.
  // - Releases its WriterID in the SMB arbiter.
  // - The release can let the endpoint destroy this object. So the call must
  //   be the writer's last access to it.
  void OnWriterDestroyed(WriterID);

  // The view that writers borrow, set once and then fixed.
  SharedRingBuffer* ring_buffer() {
    return ring_buffer_ ? &*ring_buffer_ : nullptr;
  }

  // Drain requests, from writers on their own thread:

  // Asks the service to read the ring buffer. See DrainUrgency.
  void RequestDrain(DrainUrgency);

  // Requests a drain after the writer publishes its data. Then runs |callback|
  // on the endpoint thread. See the class comment for the order.
  // - Does not wait for a service ack.
  // - If the endpoint destroys this object before the drain task runs, the
  //   task sends nothing. The endpoint thread still runs |callback|.
  void Flush(std::function<void()>);

 private:
  friend class test::ProducerRingBufferArbiterTestPeer;

  // Creates the ring buffer for the first v2 instance, and attaches it.
  // A failure is final: the state moves to kDetached, and the writers of all
  // v2 instances are NullTraceWriters, with no v1 fallback.
  void CreateAndAttachRingBuffer(size_t size_budget);
  // The attach reply calls it when the service accepts the ring buffer.
  // It moves kPending to kAttached, and does nothing in kDetached, because
  // over IPC the accept reply can arrive after a disconnect.
  void OnReaderAttached();
  // Runs on the endpoint thread. CHECKs that the transition is valid.
  void SetReaderState(ReaderState);

  // Posts a drain task to the endpoint thread. Callable from any thread.
  // - Without |force|, requests share a pending task.
  // - |force| posts a new task so Flush() can place its callback after it.
  // - The task clears |drain_task_pending_|, then sends DrainV2RingBuffer.
  void PostDrainTask(bool force);

  // --- Borrowed. They must outlive this object. ---

  // Runs tasks on the endpoint thread. Writers post drain and flush
  // requests here.
  base::TaskRunner* const task_runner_;
  // Sends AttachV2RingBuffer and DrainV2RingBuffer to the service. Endpoint
  // thread only.
  ProducerEndpoint* const endpoint_;

  // Allocates |memory_| for the first v2 instance. See AllocateV2RingBufferFn.
  const AllocateV2RingBufferFn allocate_v2_ring_buffer_;

  // --- Ring buffer mapping and view. Set once, by the first v2 instance. ---
  //
  // Only the endpoint thread sets them, before |reader_state_| leaves
  // kNoRingBuffer.
  // Writers exist only after that, so they read them without a lock.

  // The SMB arbiter. Any thread allocates and releases WriterIDs here.
  SharedMemoryArbiter* shared_memory_arbiter_ = nullptr;
  // The shared memory that holds the ring buffer. Declared before
  // |ring_buffer_|, which points into it.
  //
  // Mapping ownership depends on the transport:
  // - In-process, producer and service share this pointer. The mapping stays
  //   alive until both release it.
  // - Over IPC, the service maps the fd separately and owns that mapping.
  std::shared_ptr<SharedMemory> memory_;
  // The view that writers borrow.
  std::optional<SharedRingBuffer> ring_buffer_;

  // --- Shared with writer threads. ---

  // Writers read it from any thread. Only SetReaderState() writes it.
  std::atomic<ReaderState> reader_state_{ReaderState::kNoRingBuffer};
  // Nonzero while a shared drain task is pending. A request without |force|
  // sets it before it posts that task. The task clears it before it sends
  // DrainV2RingBuffer.
  std::atomic<uint32_t> drain_task_pending_{0};

  // Guards |v2_ring_buffer_instances_|, which only the endpoint thread writes
  // but MaybeCreateTraceWriter() reads from any thread.
  base::MaybeRtMutex mutex_;
  // The running data source instances that use the v2 ring buffer.
  base::FlatSet<DataSourceInstanceID> v2_ring_buffer_instances_;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Any thread can call GetWeakPtr(). Only the endpoint thread dereferences
  // the weak pointers.
  base::WeakPtrFactory<ProducerRingBufferArbiter> weak_factory_{this};
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PRODUCER_RING_BUFFER_ARBITER_H_
