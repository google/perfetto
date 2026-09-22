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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_ARBITER_IMPL_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_ARBITER_IMPL_H_

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto {
class SharedMemoryArbiter;
namespace tracing_v2 {

// The endpoint owns this arbiter. Ring writers reserve IDs in the SMB arbiter,
// so its TryShutdown keeps the endpoint and this mapping alive until they die.
// The endpoint creates this object before any data source and never resets it.
// Setup, state transitions, and callbacks use the endpoint thread. Packet
// writes use atomic state and notification.
//
// The following state transitions are possible:
//
//       OfferRingBuffer() succeeds          SetAccepted()
//   [ kIdle ] -----------------> [ kOffered ] ----------> [ kAccepted ]
//       |                              |                       |
//       +------------------------------+-----------------------+
//                                      | Disconnect()
//                                      v
//                              [ kDisconnected ]
//                                  (terminal)
//
// In kOffered, callers can create writers and publish data before the service
// confirms the offer. The service can still reject the offer at this point.
// NotifyReader(), TryMakeReaderProgress(), and the drain in Flush() require
// kAccepted. Before acceptance, the service's reader might not be ready.
class SharedRingBufferArbiterImpl : public SharedRingBufferWriter::Delegate {
 public:
  enum class State {
    // Construction starts here with no ring. No transition returns here.
    kIdle,
    // OfferRingBuffer() succeeds from kIdle. Writers can publish, but the
    // arbiter still needs the service's confirmation before reader requests.
    kOffered,
    // SetAccepted() confirms the service's reader from kOffered.
    kAccepted,
    // Disconnect() enters this terminal state from any state, including itself.
    // Invalid input to OfferRingBuffer() also disconnects from kIdle.
    // New writers, chunk acquisitions, and reader requests stop.
    kDisconnected,
  };

  // The endpoint constructs this on its thread. The caller keeps all arguments
  // valid until destruction and retains their ownership.
  SharedRingBufferArbiterImpl(base::TaskRunner*,
                              ProducerEndpoint*,
                              SharedMemoryArbiter*);
  // The endpoint destroys this on its thread after all writers release IDs.
  // Disconnect() posts any queued flush callbacks before destruction.
  ~SharedRingBufferArbiterImpl() override;

  // Writers retain this address for their lifetime. Copies cannot replace it.
  SharedRingBufferArbiterImpl(const SharedRingBufferArbiterImpl&) = delete;
  SharedRingBufferArbiterImpl& operator=(const SharedRingBufferArbiterImpl&) =
      delete;

  // The endpoint supplies the mapping on its thread before service acceptance.
  // In kIdle, takes ownership of a valid mapping or disconnects on invalid
  // input. Returns false outside kIdle without a state change. Existing writers
  // keep access to the view and mapping after rejection.
  bool OfferRingBuffer(std::unique_ptr<SharedMemory>, uint32_t chunk_size);

  // Any thread can create a writer for this instance and target buffer.
  // The caller owns the returned writer. Two failure paths differ:
  // - Outside kOffered and kAccepted, returns null. The arbiter no longer
  //   accepts new writers.
  // - When the SMB arbiter cannot allocate a WriterID (exhaustion or
  //   shutdown), returns a NullTraceWriter. The caller can still use and
  //   destroy it, but no packets reach the ring.
  std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID,
                                                 BufferExhaustedPolicy);

  // The endpoint confirms service acceptance on its thread. This requests a
  // reader notification and submits queued flushes. Does nothing outside
  // kOffered.
  void SetAccepted();

  // The endpoint calls this on its thread after rejection or disconnect.
  // Enters the terminal state and posts queued flush callbacks without a drain.
  // Repeated calls are safe. Existing writers retain the mapping.
  void Disconnect();

  // Any writer thread can request a drain after service acceptance.
  // Coalesces requests into one pending task on the endpoint thread.
  void NotifyReader() override;

  State state() const;

  // Writer threads can acquire chunks once the ring exists and until
  // disconnect.
  bool CanAcquireChunks() const override;

  // Writer threads can wait for space once the service confirms its reader.
  // Returns false after disconnect.
  bool ShouldWaitForReader() const override;

  // A writer requests a drain for each wait after service acceptance.
  // Runs synchronously on the endpoint thread. Other threads post a task for
  // each request. This path does not invoke application callbacks.
  void TryMakeReaderProgress() override;

  // A writer calls this on its thread after it publishes its data.
  // An empty callback only requests NotifyReader(), with no acknowledgement.
  // A nonempty callback runs once on the endpoint thread on every path:
  // - In kOffered, queues the flush until acceptance or disconnect.
  // - In kAccepted, drains and waits for service acknowledgement.
  // - In kIdle or kDisconnected, completes without a drain.
  // If the posted task outlives this arbiter, it invokes the callback directly.
  void Flush(std::function<void()>);

  // The writer calls this on its thread after its final publication.
  // Releases its WriterID in the SMB arbiter. This can allow the endpoint and
  // this arbiter to die. Do not access members after the release.
  void OnWriterDestroyed(WriterID);

  // The endpoint thread can borrow the view until this arbiter's destruction.
  // Returns null before a successful OfferRingBuffer().
  SharedRingBuffer* ring() { return ring_.get(); }

 private:
  void SetState(State);
  // The endpoint task clears the pending flag and drains if still accepted.
  void SendNotification();
  // The endpoint thread requests progress without application callbacks.
  void DrainForStall();
  // The endpoint thread queues or submits a flush according to the state.
  void FlushOnEndpoint(std::function<void()>);

  // Borrowed endpoint runner. Writer threads post reader and flush requests.
  base::TaskRunner* const task_runner_;
  // Borrowed owner. Only the endpoint thread calls its transport methods.
  ProducerEndpoint* const endpoint_;
  // Borrowed SMB arbiter. Writer threads reserve IDs here to prevent shutdown.
  SharedMemoryArbiter* const shared_memory_arbiter_;

  // keeps the writers' mapping alive until destruction.
  std::unique_ptr<SharedMemory> memory_;
  // owns the view that writers borrow after creation.
  std::unique_ptr<SharedRingBuffer> ring_;
  // Atomic because packet-writing threads read the lifecycle without the lock.
  std::atomic<State> state_{State::kIdle};
  // Atomic flag to coalesce notifications from writers onto the endpoint
  // thread.
  std::atomic<bool> notification_pending_{false};

  // Endpoint thread: callbacks for offered flushes.
  std::vector<std::function<void()>> pending_flushes_;
  // Checks that setup, state transitions, and callbacks use the endpoint
  // thread.
  PERFETTO_THREAD_CHECKER(thread_checker_)
  // Any thread can copy weak pointers. The endpoint thread dereferences them.
  base::WeakPtrFactory<SharedRingBufferArbiterImpl> weak_factory_{this};
};

}  // namespace tracing_v2
}  // namespace perfetto
#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_ARBITER_IMPL_H_
