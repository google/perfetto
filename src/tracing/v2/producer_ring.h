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
// The caller provides SharedMemory for either the in-process or system backend.
// ProducerRing owns the ring view, allocates independent v2 writer IDs, and
// implements TraceWriterV2Impl::Delegate for those writers.
//
// Writers retain the ProducerRing and its memory through std::shared_ptr.
// On disconnect, the muxer calls DetachFromService(). Surviving writers can
// still write to the ring, but their notifications no longer reach the service.
//
// CreateTraceWriter() and delegate callbacks can run concurrently on different
// writer threads. |mutex_| guards writer-ID allocation, notifications and
// detach state. ServiceChannel directs notifications to the service sequence.
class ProducerRing : public TraceWriterV2Impl::Delegate,
                     public std::enable_shared_from_this<ProducerRing> {
 public:
  // The producer's control channel to traced. Methods accept calls from any
  // writer thread. The implementation forwards them to the service sequence,
  // through direct calls or IPC. In process, a call on that sequence can drain
  // inline. Successful replies must be asynchronous because the ring holds
  // |mutex_| during these calls.
  class ServiceChannel {
   public:
    virtual ~ServiceChannel();

    // Asks the service to drain published ring data into the trace buffer.
    // After the drain, |on_picked_up| permits another coalesced notification.
    // The callback can be empty.
    virtual void NotifyRingData(std::function<void()> on_picked_up) = 0;
    // Replies successfully after the service consumes final data and records
    // retirement. A false reply leaves the ID reserved until disconnect.
    virtual void RetireWriter(WriterID, std::function<void(bool)> callback) {
      callback(false);
    }

    // True if a wait on this thread prevents the drain task from executing.
    // A stalling writer then drops data. The default is false.
    virtual bool DrainRunsOnCurrentThread() { return false; }
  };

  // Creates a ring over |memory|, which must be at least
  // LogicalSize(num_chunks, chunk_size) bytes and zero-filled. |channel| must
  // outlive the ring's control use (until DetachFromService()). Returns null if
  // the geometry is invalid.
  // A null channel permits early writes. Attach after adoption succeeds. Until
  // then, a full ring drops data because no reader can release space.
  //
  // Writers retain |memory| through ProducerRing after disconnect.
  // In process, the service endpoint also holds a shared_ptr during reads.
  static std::shared_ptr<ProducerRing> Create(
      std::shared_ptr<SharedMemory> memory,
      uint32_t num_chunks,
      uint32_t chunk_size,
      ServiceChannel* channel);

  ~ProducerRing() override;

  ProducerRing(const ProducerRing&) = delete;
  ProducerRing& operator=(const ProducerRing&) = delete;

  // Exact extent in bytes: ring header plus chunk area. OS page rounding for
  // SharedMemory does not affect this value.
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
  // Sends retirement under |mutex_|. Successful replies must be asynchronous.
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
  // DetachFromService() clears this pointer under |mutex_|.
  ServiceChannel* channel_;
  bool was_attached_;
  // Independent of v1 allocation. Retirement separates retained incarnations
  // before the producer can reuse an ID.
  IdAllocator<WriterID> writer_ids_{kMaxWriterID};
  std::vector<WriterID> retired_before_attach_;
  // Count of CreateTraceWriter() calls that exhausted the ID space.
  // Existing writers remain usable.
  uint64_t writer_id_exhausted_count_ = 0;

  // |mutex_| guards these NotifyReaderBatched() flags.
  // |batched_notify_in_flight_| marks an outstanding coalesced drain.
  // |batched_notify_dirty_| records packets that finalized during that drain.
  // Its reply then triggers one more notification to drain those packets.
  // NotifyReader() bypasses these flags to request immediate progress.
  bool batched_notify_in_flight_ = false;
  bool batched_notify_dirty_ = false;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PRODUCER_RING_H_
