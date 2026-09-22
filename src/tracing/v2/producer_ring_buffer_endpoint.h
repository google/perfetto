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

#ifndef SRC_TRACING_V2_PRODUCER_RING_BUFFER_ENDPOINT_H_
#define SRC_TRACING_V2_PRODUCER_RING_BUFFER_ENDPOINT_H_

#include <stdint.h>

#include <atomic>
#include <functional>
#include <memory>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto {
class SharedMemoryArbiter;
namespace tracing_v2 {

namespace test {
class ProducerRingBufferEndpointTestPeer;
}  // namespace test

// The producer side of one tracing v2 ring buffer. The service has its own
// side, which reads the ring buffer.
//
// It is not the buffer: SharedMemory holds the bytes, and SharedRingBuffer is
// the view of them. This class owns both on the producer side.
//
// The producer's ProducerEndpoint (for example ProducerIPCClientImpl) creates
// one for each ring buffer that it shares with the service. In this file,
// "endpoint" alone means that ProducerEndpoint. For the path of packet bytes,
// see TraceWriterV2Impl.
//
// Why this class exists:
//
// Writers put packet bytes into the ring buffer without this class. Unlike
// v1, they do not ask an arbiter for each chunk. Some jobs remain that one
// writer cannot do alone. This class does them for all writers:
//
//   Job                             Why a writer cannot do it
//   ------------------------------  -------------------------------------
//   Ask the service to read the     Only the endpoint thread sends IPC.
//   ring buffer (DrainRingBuffer).  Writers run on any thread.
//
//   Merge drain requests.           Each writer sees only its own data.
//                                   One shared flag merges the requests
//                                   of all writers into one drain.
//
//   Track the service reader        The service accepts or rejects the
//   (see ReaderState).              ring buffer once, for all writers.
//
//   Own the ring buffer mapping.    Writers only borrow the memory.
//
//   Give out WriterIDs.             IDs come from the pool of the SMB
//                                   arbiter, shared with v1 writers.
//
// Threads:
//
// Writers call this class from any thread. All other work runs on the
// endpoint thread: creation, state changes, flush callbacks and service
// requests. A writer call that needs the endpoint thread posts a task to it.
//
// The diagrams below show each flow. Time goes down.
// - "--->" is a posted task or an IPC request.
// - "***>" is a change in shared memory, with no message.
//
// 1. After each publication, the writer asks for a drain:
//
//     writer                 endpoint                 service
//        |                       |                       |
//        | NotifyReader(         |                       |
//        |   kPositionsReady)    |                       |
//        |------ post task ----->|                       |
//        |                       | SendDrainRequest()    |
//        |                       |--- DrainRingBuffer -->| copies published
//        |                       |                       | chunks
//
//    - If a task is already pending, the writer does not post another.
//    - Before kAttached, NotifyReader() does nothing.
//
// 2. If the ring buffer is full, the writer asks for a drain, then waits:
//
//     writer                 endpoint                 service
//        |                       |                       |
//        | NotifyReader(         |                       |
//        |   kWriterStalled)     |                       |
//        |------ post task ----->|                       |
//        |                       | SendDrainRequest()    |
//        |                       |--- DrainRingBuffer -->| copies chunks
//        | waits for space       |                       |
//        |<*************** read_pos moves ***************|
//
//    - The writer repeats the request before each wait.
//    - On the endpoint thread, this class sends DrainRingBuffer at once, in
//      the writer's call. A posted task could not run while the writer
//      waits on that thread.
//    - Before kAttached, the writer drops the packet and does not wait.
//
// 3. Flush(callback) asks for a drain, then runs |callback|:
//
//     writer                 endpoint                 service
//        |                       |                       |
//        | Flush(callback)       |                       |
//        |------ post task ----->|                       |
//        |                       |--- DrainRingBuffer -->| copies chunks
//        |                       | callback()            |
//
//    - Flush() posts its own task. The task sends the drain request, then
//      runs the callback.
//    - Flush() does not use the drain task that writers share. That task
//      can come late. See PostDrainTask() in the .cc file.
//    - Without a callback, Flush() only asks for a drain, like a writer
//      after a publication.
//    - The callback does not wait for the service. There is no ack.
//    - A message that the callback sends reaches the service after the
//      drain request. The service handles them in order.
//    - Before kAttached, there is no drain. The callback still runs.
//
// 4. When a writer is destroyed, it calls OnWriterDestroyed() on its own
//    thread. This releases its WriterID in the SMB arbiter. No task is
//    posted.
//
// Ownership:
//
// "owns" is a unique_ptr. "uses" is a raw pointer.
//
//   endpoint (for example ProducerIPCClientImpl)
//     |-- owns --> SharedMemoryArbiterImpl: the WriterID pool
//     |-- owns --> ProducerRingBufferEndpoint (this class)
//                    |-- owns --> SharedMemory: the ring buffer mapping
//                    |-- owns --> SharedRingBuffer: the view
//                    |-- uses --> endpoint, SharedMemoryArbiterImpl
//
//   data source (SDK)
//     |-- owns --> TraceWriterV2Impl
//                    |-- uses --> ProducerRingBufferEndpoint
//                    |-- holds -> a WriterID from SharedMemoryArbiterImpl
//                    |-- owns --> SharedRingBufferWriter
//                                   |-- uses --> SharedRingBuffer
//
// Writers use raw pointers into objects that the endpoint owns. The WriterID
// keeps these pointers valid:
// - While a writer holds a WriterID, TryShutdown() of the SMB arbiter fails.
// - So the endpoint keeps the SMB arbiter, this object and the mapping.
// - The writer releases its WriterID last, in OnWriterDestroyed().
class ProducerRingBufferEndpoint : public SharedRingBufferWriter::Delegate {
 public:
  // Tells if a service reader drains the ring buffer. Only the endpoint
  // thread changes the state. Writers read it from any thread.
  //
  //     Create()             OnReaderAttached()
  //   ----------> [ kPending ] ------------------> [ kAttached ]
  //                    |                                |
  //                    +---------------+----------------+
  //                                    | Disconnect()
  //                                    v
  //                              [ kDetached ]
  //                               (terminal)
  enum class ReaderState {
    // No reader yet. The endpoint shared the ring buffer and waits for the
    // service reply.
    // - Writers can already publish.
    // - The service can still reject the ring buffer.
    // - No drain requests go to the service.
    // - A writer does not wait for space in a full ring buffer. It drops the
    //   packet instead, also under kStall.
    kPending,

    // The service accepted the ring buffer. Its reader drains it.
    // - Drain requests start.
    // - A writer can wait for space in a full ring buffer.
    kAttached,

    // Terminal. No reader now or later. Entered after rejection, disconnect
    // or destruction of this class.
    // - New writers are NullTraceWriters. Drain requests stop.
    // - Existing writers keep the mapping. They drop packets when the ring
    //   buffer is full.
    kDetached,
  };

  // Takes ownership of the producer's ring buffer mapping. The endpoint calls
  // this on its thread, then shares the mapping with the service.
  // - Returns null if the mapping is null, misaligned or larger than
  //   kMaxShmSize, or if |chunk_size| gives no valid layout.
  // - The other arguments are borrowed and must outlive this object.
  static std::unique_ptr<ProducerRingBufferEndpoint> Create(
      base::TaskRunner*,
      ProducerEndpoint*,
      SharedMemoryArbiter*,
      std::unique_ptr<SharedMemory> ring_buffer_memory,
      uint32_t chunk_size);

  // The endpoint destroys this on its thread after all writers release their
  // IDs. Calls Disconnect().
  ~ProducerRingBufferEndpoint() override;

  // Writers keep a pointer to this object.
  ProducerRingBufferEndpoint(const ProducerRingBufferEndpoint&) = delete;
  ProducerRingBufferEndpoint& operator=(const ProducerRingBufferEndpoint&) =
      delete;

  // Endpoint thread:

  // The endpoint calls this on its thread when the service accepts the ring
  // buffer. Requests a drain. Ignored after
  // Disconnect(), because the accept reply can arrive after it.
  void OnReaderAttached();

  // The endpoint calls this on its thread after rejection or disconnect.
  // Enters kDetached. Repeated calls are safe.
  void Disconnect();

  // Any thread:

  // Creates a writer for |target_buffer|. Any thread can call this. The
  // caller owns the writer.
  //
  // Never returns null. Returns a NullTraceWriter, which discards all
  // packets, if:
  // - the state is kDetached, or
  // - the SMB arbiter has no free WriterID (exhaustion or shutdown).
  std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID target_buffer,
                                                 BufferExhaustedPolicy);

  // Writers, on their own thread:

  // A writer calls this on its thread after it publishes its data. Asks for
  // a drain, then runs |callback| on the endpoint thread.
  // - |callback| does not wait for a service ack. It runs after the drain
  //   request is sent.
  // - Without a reader, there is no drain. |callback| still runs, also if
  //   this object is destroyed first.
  void Flush(std::function<void()>);

  // The writer calls this on its thread after its final publication.
  // Releases its WriterID in the SMB arbiter. The release can let the
  // endpoint destroy this object, so the call must be the writer's last
  // access to it.
  void OnWriterDestroyed(WriterID);

  // SharedRingBufferWriter::Delegate:

  // Merges requests from all writer threads into one pending drain task.
  // For kWriterStalled on the endpoint thread, sends the drain request at
  // once instead. Does nothing outside kAttached. Never runs application
  // callbacks.
  void NotifyReader(NotifyReason) override;

  // True in kAttached only.
  bool IsReaderAttached() const override;

 private:
  friend class test::ProducerRingBufferEndpointTestPeer;

  ProducerRingBufferEndpoint(base::TaskRunner*,
                             ProducerEndpoint*,
                             SharedMemoryArbiter*,
                             std::unique_ptr<SharedMemory> ring_buffer_memory,
                             uint32_t chunk_size);

  // Runs on the endpoint thread. CHECKs that the transition is valid.
  void SetReaderState(ReaderState);
  // Any thread. Posts SendDrainRequest(), unless a drain task is already
  // pending.
  void PostDrainTask();
  // Runs on the endpoint thread. Clears the pending flag, then sends
  // DrainRingBuffer.
  void SendDrainRequest();

  // --- Borrowed. They must outlive this object. ---

  // Runs tasks on the endpoint thread. Writers post drain and flush
  // requests here.
  base::TaskRunner* const task_runner_;
  // Sends DrainRingBuffer to the service. Endpoint thread only.
  ProducerEndpoint* const endpoint_;
  // The SMB arbiter. Any thread reserves and releases WriterIDs here.
  SharedMemoryArbiter* const shared_memory_arbiter_;

  // --- Owned ring buffer. Fixed after construction. ---

  // Keeps the writers' mapping alive until this object is destroyed.
  // Declared before |ring_buffer_|, which points into it.
  const std::unique_ptr<SharedMemory> memory_;
  // The view that writers borrow.
  const std::unique_ptr<SharedRingBuffer> ring_buffer_;

  // --- Shared with writer threads. ---

  // Writers read it from any thread. Only SetReaderState() writes it.
  std::atomic<ReaderState> reader_state_{ReaderState::kPending};
  // True while a drain task is pending. Merges writer requests.
  std::atomic<bool> drain_task_pending_{false};

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Posted tasks use weak pointers to detect that this object is gone.
  // Any thread can copy them. Only the endpoint thread uses them.
  base::WeakPtrFactory<ProducerRingBufferEndpoint> weak_factory_{this};
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PRODUCER_RING_BUFFER_ENDPOINT_H_
