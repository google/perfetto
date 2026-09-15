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

#ifndef SRC_TRACING_V2_PRODUCER_RING_H_
#define SRC_TRACING_V2_PRODUCER_RING_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/core/id_allocator.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/trace_writer_v2_impl.h"

namespace perfetto {

class SharedMemory;

namespace tracing_v2 {

// Producer-side owner of a tracing v2 shared ring.
//
// It does not allocate the ring itself: the caller (the muxer, per backend)
// provides a SharedMemory, so the same component serves both the in-process and
// system-IPC backends. It owns the ring view over that memory, hands out v2
// TraceWriters with independent writer ids, and is the writers' shared delegate
// (TraceWriterV2Impl::Delegate).
//
// Lifetime: writers hold a std::shared_ptr to the ProducerRing, so the ring and
// its backing memory outlive the service endpoint while any writer is still
// alive. On disconnect the muxer calls DetachFromService(): after that, writer
// notifications no longer touch the (possibly destroyed) service, but writers
// can keep writing to the ring until they are destroyed.
//
// Threading: CreateTraceWriter() and the TraceWriterV2Impl::Delegate callbacks
// (NotifyReader/Flush/OnWriterDestroyed) may run on any writer thread and may
// overlap; the writer-id allocation and the notification/detach state are
// guarded by |mutex_|. The service notification itself is posted to the service
// via the ServiceChannel and executed on the service sequence.
class ProducerRing : public TraceWriterV2Impl::Delegate,
                     public std::enable_shared_from_this<ProducerRing> {
 public:
  // The producer's control channel to traced. The ring calls these; the
  // implementation forwards to the service (a direct call in-process, an IPC
  // notification for the system backend). Its methods may be called from any
  // thread and must post to the service sequence themselves.
  class ServiceChannel {
   public:
    virtual ~ServiceChannel();

    // Asks the service to read newly published ring data. The service, on its
    // own sequence, drains the ring into the trace buffer and then runs
    // |on_picked_up| (used to re-arm coalescing). |on_picked_up| may be empty.
    virtual void NotifyRingData(std::function<void()> on_picked_up) = 0;
    // Replies asynchronously after final data is consumed and retirement is
    // recorded. A false reply leaves the ID reserved until disconnect.
    virtual void RetireWriter(WriterID, std::function<void(bool)> callback) {
      callback(false);
    }

    // True if the service drains this ring on the calling thread, so a writer
    // that parks waiting for space would deadlock the drain. Lets a stalling
    // writer drop instead. The default is false (the drain runs elsewhere).
    virtual bool DrainRunsOnCurrentThread() { return false; }
  };

  // Creates a ring over |memory|, which must be at least
  // LogicalSize(num_chunks, chunk_size) bytes and zero-filled. |channel| must
  // outlive the ring's control use (until DetachFromService()). Returns null if
  // the geometry is invalid.
  // A null channel permits early writes. Attach after adoption succeeds. Until
  // then, a full ring drops data because no reader can release space.
  //
  // Ownership of |memory| is shared: writers keep the ProducerRing (and hence
  // the memory) alive after disconnect, and for the in-process backend the
  // service endpoint holds the same shared_ptr while it reads the ring.
  static std::shared_ptr<ProducerRing> Create(
      std::shared_ptr<SharedMemory> memory,
      uint32_t num_chunks,
      uint32_t chunk_size,
      ServiceChannel* channel);

  ~ProducerRing() override;

  ProducerRing(const ProducerRing&) = delete;
  ProducerRing& operator=(const ProducerRing&) = delete;

  // Exact logical byte extent of a ring with this geometry (ring header plus
  // the chunk area), independent of any OS mapping/page rounding of the
  // SharedMemory.
  static size_t LogicalSize(uint32_t num_chunks, uint32_t chunk_size);

  // Creates a v2 TraceWriter targeting |target_buffer|. IDs become reusable
  // after the service drains final data and acknowledges retirement. Returns a
  // NullTraceWriter if all IDs are active or await retirement. Thread-safe.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy buffer_exhausted_policy);

  // The ring view and backing memory, for the service to adopt. shared_memory()
  // returns a new reference the caller can hand to the service endpoint (see
  // Create()'s shared-ownership note).
  SharedRingBuffer* ring_buffer() { return &ring_; }
  const std::shared_ptr<SharedMemory>& shared_memory() const { return memory_; }
  uint32_t num_chunks() const { return num_chunks_; }
  uint32_t chunk_size() const { return chunk_size_; }

  // Stops using the ServiceChannel. Called on the muxer sequence at disconnect.
  // After this, NotifyReader()/Flush() no longer notify the service, but the
  // ring stays valid for surviving writers. Idempotent.
  void DetachFromService();
  // Connects a ring created without a channel after successful adoption.
  // A detached connection cannot attach again. Its surviving writers keep
  // the old mapping and must not enter a new connection's identity namespace.
  bool AttachToService(ServiceChannel*);

 private:
  ProducerRing(std::shared_ptr<SharedMemory> memory,
               uint32_t num_chunks,
               uint32_t chunk_size,
               ServiceChannel* channel);

  // TraceWriterV2Impl::Delegate (and SharedRingBufferWriter::Delegate):
  void NotifyReader() override;
  void NotifyReaderBatched() override;
  bool DrainRunsOnCurrentThread() override;
  void Flush(WriterID, std::function<void()> callback) override;
  void OnWriterDestroyed(WriterID) override;
  // Sends retirement while holding |mutex_|. Successful replies must be async.
  void RetireWriterLocked(WriterID);

  // Sends one coalesced steady-state notification. |mutex_| must be held.
  void SendBatchedNotifyLocked();
  // Runs after the service drained a coalesced notification. Re-arms coalescing
  // and rechecks for packets finalized while the drain was in flight.
  void OnBatchedNotifyDrained();

  const std::shared_ptr<SharedMemory> memory_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
  SharedRingBuffer ring_;  // A view over |memory_|.

  std::mutex mutex_;
  // Cleared by DetachFromService(); guarded by |mutex_|.
  ServiceChannel* channel_;
  bool was_attached_;
  // Independent of v1 allocation. Retirement separates retained incarnations
  // before the producer can reuse an ID.
  IdAllocator<WriterID> writer_ids_{kMaxWriterID};
  std::vector<WriterID> retired_before_attach_;
  // Count of CreateTraceWriter() calls refused because the id space was
  // exhausted. Existing writers keep working; only new writers are refused.
  uint64_t writer_id_exhausted_count_ = 0;

  // Coalescing state for the steady-state NotifyReaderBatched() path, guarded
  // by |mutex_|. |batched_notify_in_flight_| is true while a coalesced drain is
  // outstanding; |batched_notify_dirty_| records that another packet finalized
  // meanwhile, so the drain's ack sends one more notification (the recheck).
  // The progress-critical NotifyReader() path never uses these and always
  // notifies immediately.
  bool batched_notify_in_flight_ = false;
  bool batched_notify_dirty_ = false;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PRODUCER_RING_H_
