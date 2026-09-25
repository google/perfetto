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
#include <map>
#include <memory>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/rt_mutex.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto {
class SharedMemoryArbiter;
namespace tracing_v2 {

using ChunkSizeOption =
    protos::gen::DataSourceConfig_ExperimentalTracingV2Config_ChunkSizeOption;

// Picks a chunk size from |options| by weight. |random| is a uniform random
// number. An absent weight counts as 1. Options with an invalid size are
// skipped: the service rejects them, but an older service does not. Returns
// 256 if no option can be picked.
uint32_t PickChunkSize(const std::vector<ChunkSizeOption>& options,
                       uint64_t random);

namespace test {
class ProducerRingBufferArbiterTestPeer;
}  // namespace test

// Coordinates the producer writers that share one tracing v2 ring buffer, for
// one producer connection. The service has its own reader for the ring buffer.
//
// SharedMemory holds the bytes, and SharedRingBuffer provides a view of them.
// This class owns the view and shares ownership of the mapping.
//
// The producer's ProducerEndpoint owns one:
// - ProducerIPCClientImpl over IPC: always.
// - ProducerEndpointImpl in-process: only if the connection supports v2.
// Both use it the same way. They differ only in the memory (a sealed memfd
// over IPC), and in whether AttachV2RingBuffer() and DrainV2RingBuffer() are
// an IPC or a direct call.
//
// In this file, "endpoint" means that ProducerEndpoint, and "SMB arbiter"
// means the SharedMemoryArbiter used for v1 tracing. For the path of packet
// bytes, see TraceWriterV2Impl.
//
// Why this class exists:
//
// Writers put packet bytes into the ring buffer without this class. Unlike
// v1, they do not ask an arbiter for each chunk. Some jobs remain that one
// writer cannot do alone. This class does them for all writers:
//
//   Job                             Why a writer cannot do it
//   ------------------------------  -------------------------------------
//   Select v2 for each data         The choice is per instance, and all
//   source instance.                its writers must get the same one.
//
//   Create the ring buffer and      There is one ring buffer for the
//   attach it to the service.       whole connection.
//
//   Track the service reader        The service accepts or rejects the
//   (see State).                    ring buffer once, for all writers.
//
//   Ask the service to read the     Only the endpoint thread sends IPC.
//   ring buffer                     Writers run on any thread.
//   (DrainV2RingBuffer).
//
//   Merge drain requests.           Each writer sees only its own data.
//                                   One shared flag merges the requests
//                                   of all writers into one drain.
//
//   Give out WriterIDs.             IDs come from the pool of the SMB
//                                   arbiter, shared with v1 writers.
//
// Threads:
//
// Writers, and MaybeCreateTraceWriter(), call this class from any thread. All
// other work runs on the endpoint thread: setup, state changes, flush
// callbacks and service requests. A writer call that needs the endpoint
// thread posts a task to it.
//
// The diagrams below show each flow. Time goes down.
// - "--->" is a direct call on the writer thread.
// - "===>" is a posted task or an IPC request.
// - "***>" is a change in shared memory, with no message.
//
// 1. The first data source instance that uses v2 creates the ring buffer:
//
//     endpoint                   this class                service
//        |                           |                        |
//        | SetupInstance()           |                        |
//        |-------------------------->| allocates the memory,  |
//        |                           | kNoRingBuffer ->       |
//        |                           | kPending               |
//        |                           |                        |
//        |                           |== AttachV2RingBuffer =>| maps, checks
//        |                           |<===== reply ===========| attaches
//        |                           | kAttached, or          |
//        |                           | kDetached if rejected  |
//
//    - Later instances reuse the ring buffer.
//    - If the allocation fails, the state stays kNoRingBuffer, and the next
//      instance that uses v2 tries again.
//
// 2. After a publication, the writer asks for a drain if the ring buffer's
//    outstanding positions reach the drain threshold. The default is a
//    quarter of the ring buffer:
//
//     writer                  arbiter                 service
//        |                       |                       |
//        | NotifyReader(         |                       |
//        |   kPositionsReady)    |                       |
//        |---------------------->| posts a task          |
//        |                       |                       |
//        |                       | SendDrainRequest()    |
//        |                       |== DrainV2RingBuffer =>| copies published
//        |                       |                       | chunks
//
//    - If a drain task is already pending, NotifyReader() posts nothing.
//    - The service handles attach and drain requests in order. It ignores
//      drains if it has not accepted the ring buffer.
//
// 3. If the ring buffer is full, the writer asks for a drain, then waits:
//
//     writer                  arbiter                 service
//        |                       |                       |
//        | NotifyReader(         |                       |
//        |   kWriterStalled)     |                       |
//        |---------------------->| posts a task          |
//        |                       |                       |
//        |                       | SendDrainRequest()    |
//        |                       |== DrainV2RingBuffer =>| copies chunks
//        | waits for space       |                       |
//        |<*************** read_pos moves ***************|
//
//    - The writer repeats the request before each wait.
//    - On the endpoint thread, this class sends DrainV2RingBuffer at once, in
//      the writer's call. A posted task could not run while the writer
//      waits on that thread.
//    - In kPending, the writer drops the packet and does not wait.
//
// 4. Flush(callback) asks for a drain, then runs |callback|:
//
//     writer or endpoint      arbiter                 service
//        |                       |                       |
//        | Flush(callback)       |                       |
//        |---------------------->| posts a drain task,   |
//        |                       | then the callback     |
//        |                       |                       |
//        |                       | SendDrainRequest()    |
//        |                       |== DrainV2RingBuffer =>| copies chunks
//        |                       | callback()            |
//
//    - Flush() queues its own drain task before queuing the callback.
//      The Flush() comment in the .cc file explains why sharing a pending
//      drain task could let the callback run first.
//    - The callback runs after sending the request, without a service ack.
//    - A message that the callback sends reaches the service after the
//      drain request. The service handles them in order.
//    - The callback still runs if this object is destroyed first.
//
// 5. When a writer is destroyed, it calls OnWriterDestroyed() on its own
//    thread. This releases its WriterID in the SMB arbiter. No task is
//    posted.
//
// Ownership:
//
// "owns" is a unique_ptr or a member. "shares" is a shared_ptr. "uses" is a
// raw pointer.
//
//   endpoint (for example ProducerIPCClientImpl)
//     |-- owns --> SharedMemoryArbiterImpl: the WriterID pool
//     |-- owns --> ProducerRingBufferArbiter (this class)
//                    |-- shares -> SharedMemory: the ring buffer mapping
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
class ProducerRingBufferArbiter : public SharedRingBufferWriter::Delegate {
 public:
  // The ring buffer budget if the producer gives no size hint.
  static constexpr size_t kDefaultSizeBudget = 128 * 1024;

  // Allocates |size| bytes for the ring buffer. Returns null on failure.
  using AllocateMemoryFn =
      std::function<std::shared_ptr<SharedMemory>(size_t size)>;

  // The state of the ring buffer and of its service reader.
  // - Only the endpoint thread changes the state, but writers can read it
  //   from any thread.
  // - Drain requests are sent in every state, but the service ignores them
  //   if it has no accepted ring buffer.
  //
  //                    first v2 instance            accept reply
  //   [ kNoRingBuffer ] ---------------> [ kPending ] ----------> [ kAttached ]
  //          |                                |                        |
  //          +--------------------------------+------------------------+
  //                                           | rejection, Disconnect()
  //                                           v
  //                                     [ kDetached ]
  //                                      (terminal)
  enum class State {
    // No ring buffer yet: no instance used v2, or the allocation failed.
    // - The writers of a v2 instance are NullTraceWriters.
    kNoRingBuffer,

    // The ring buffer exists, and the attach request went out. No reader yet.
    // - Writers can already publish.
    // - The service can still reject the ring buffer.
    // - A writer does not wait for space in a full ring buffer. It drops the
    //   packet instead, also under kStall.
    kPending,

    // The service accepted the ring buffer. Its reader drains it.
    // - A writer can wait for space in a full ring buffer.
    kAttached,

    // Terminal. No reader now or later. Entered after rejection, disconnect
    // or destruction of this class.
    // - New writers of a v2 instance are NullTraceWriters.
    // - Existing writers keep the mapping. They drop packets when the ring
    //   buffer is full.
    kDetached,
  };

  // |endpoint| receives AttachV2RingBuffer() and the drain requests.
  // |task_runner| runs the endpoint thread. Both must outlive this object.
  ProducerRingBufferArbiter(base::TaskRunner* task_runner,
                            ProducerEndpoint* endpoint,
                            AllocateMemoryFn allocate_memory);

  // The endpoint destroys this on its thread after all writers release their
  // IDs. Enters kDetached.
  ~ProducerRingBufferArbiter() override;

  // Writers keep a pointer to this object.
  ProducerRingBufferArbiter(const ProducerRingBufferArbiter&) = delete;
  ProducerRingBufferArbiter& operator=(const ProducerRingBufferArbiter&) =
      delete;

  // Endpoint thread:

  // Call at data source setup, before the producer sees the instance.
  // - Selects v2 first if it is in |protocol_abi_versions|, the service
  //   permits it (supports_tracing_v2), and the probability sample selects it.
  // - Otherwise, the endpoint can use v1 only if it is in the common set.
  //   With no permitted version, logs an error. The endpoint returns writers
  //   that discard data.
  // - The first instance that does creates the ring buffer: at most
  //   |size_budget| bytes (kDefaultSizeBudget if zero). Its writers get
  //   WriterIDs from |arbiter|, which must outlive this object.
  void SetupInstance(
      DataSourceInstanceID,
      const DataSourceConfig&,
      const base::FlatSet<ProtocolAbiVersion>& protocol_abi_versions,
      SharedMemoryArbiter* arbiter,
      size_t size_budget);

  // Call when the producer reports that the instance stopped
  // (NotifyDataSourceStopped). Until then, also during an asynchronous stop,
  // new writers of the instance get its transport.
  void OnInstanceStopped(DataSourceInstanceID);

  // After a disconnect: no reader now or later. Enters kDetached. Existing
  // writers keep the mapping. Repeated calls are safe.
  void Disconnect();

  // Any thread:

  // Returns a writer if the instance uses the ring buffer. The caller owns
  // it. Returns null if v2 was not selected. The caller can then create a v1
  // writer only if v1 is in the common set. Otherwise, it returns a
  // NullTraceWriter.
  //
  // For a v2 instance, never returns null. Returns a NullTraceWriter, which
  // discards all packets, if:
  // - there is no ring buffer for it (kNoRingBuffer), or no reader will ever
  //   come (kDetached). There is no v1 fallback.
  // - the SMB arbiter has no free WriterID (exhaustion or shutdown).
  std::unique_ptr<TraceWriter> MaybeCreateTraceWriter(BufferID,
                                                      BufferExhaustedPolicy,
                                                      DataSourceInstanceID);

  // Requests a drain after the writer publishes its data.
  // - Always queues a drain task, even if another is pending.
  // - Runs |callback| on the endpoint thread after sending the request,
  //   without waiting for a service ack.
  // - If this object is destroyed before the task runs, skips the drain but
  //   still runs |callback|.
  // - Without a ring buffer, runs |callback| at once.
  void Flush(std::function<void()>);

  // Writers, on their own thread:

  // The writer calls this on its thread after its final publication.
  // Releases its WriterID in the SMB arbiter. The release can let the
  // endpoint destroy this object, so the call must be the writer's last
  // access to it.
  void OnWriterDestroyed(WriterID);

  // SharedRingBufferWriter::Delegate:

  // Merges requests from all writer threads into one pending drain task.
  // For kWriterStalled on the endpoint thread, sends the drain request at
  // once instead. Never runs application callbacks.
  void NotifyReader(NotifyReason) override;

  // True in kAttached only.
  bool IsReaderAttached() const override;

  // The view that writers borrow. Set once with the ring buffer, then fixed.
  // Writers exist only after it is set.
  SharedRingBuffer* ring_buffer() const { return ring_buffer_.get(); }

  State state() const { return state_.load(); }

 private:
  friend class test::ProducerRingBufferArbiterTestPeer;

  // Creates the ring buffer for the first v2 instance, and attaches it.
  void CreateAndAttachRingBuffer(const DataSourceConfig&,
                                 SharedMemoryArbiter*,
                                 size_t size_budget);
  // Runs on the endpoint thread. Ignored after Disconnect(), because the
  // reply can arrive after it.
  void OnAttachReply(bool accepted);
  // Runs on the endpoint thread. CHECKs that the transition is valid.
  void SetState(State);

  // Queues SendDrainRequest() on the endpoint thread. Callable from any thread.
  // - Ordinary notifications share a pending task.
  // - |force| queues a new task so Flush() can place its callback after it.
  void PostDrainTask(bool force);

  // Runs on the endpoint thread. Clears the pending flag, then sends
  // DrainV2RingBuffer.
  void SendDrainRequest();

  // --- Borrowed. They must outlive this object. ---

  // Runs tasks on the endpoint thread. Writers post drain and flush
  // requests here.
  base::TaskRunner* const task_runner_;
  // Receives AttachV2RingBuffer() and DrainV2RingBuffer(). Endpoint thread
  // only.
  ProducerEndpoint* const endpoint_;
  const AllocateMemoryFn allocate_memory_;

  // --- The ring buffer. Set once by the first v2 instance, then fixed. ---
  //
  // Only the endpoint thread sets them, before |state_| leaves kNoRingBuffer.
  // Writers exist only after that, so they read them without a lock.

  // The SMB arbiter. Any thread reserves and releases WriterIDs here.
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
  std::unique_ptr<SharedRingBuffer> ring_buffer_;

  // --- Shared with writer threads. ---

  // Writers read it from any thread. Only SetState() writes it.
  std::atomic<State> state_{State::kNoRingBuffer};
  // Nonzero when an ordinary notification has claimed a drain task. The
  // claiming thread may not have queued it yet. Cleared before sending a drain.
  std::atomic<uint32_t> drain_task_pending_{0};

  // |mutex_| guards |ring_buffer_instances_|.
  // - Only the endpoint thread writes it. MaybeCreateTraceWriter() reads it
  //   from any thread.
  // - Writers can be created on real-time threads, so this is a MaybeRtMutex,
  //   as in SharedMemoryArbiterImpl.
  base::MaybeRtMutex mutex_;
  // The running v2 instances, with their drain_occupancy_percent. Other
  // instances can use v1 only if it is in the connection's common set.
  std::map<DataSourceInstanceID, int32_t> ring_buffer_instances_;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Posted tasks and the attach reply use weak pointers to detect that this
  // object is gone. Any thread can copy them. Only the endpoint thread uses
  // them.
  base::WeakPtrFactory<ProducerRingBufferArbiter> weak_factory_{this};
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PRODUCER_RING_BUFFER_ARBITER_H_
