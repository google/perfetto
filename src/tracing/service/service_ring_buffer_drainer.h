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

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_runner.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"

namespace perfetto {
class TraceBufferV2;
namespace protos::pbzero {
class TracingV2RingBufferDump;
}  // namespace protos::pbzero
namespace tracing_service {
class Clock;
}  // namespace tracing_service
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

    // Called once, after the reader stopped on a protocol error. The delegate
    // can read the drainer, for example with WriteDump().
    virtual void OnRingBufferProtocolError() = 0;

    // Called for each chunk that the reader discards as malformed or in an
    // unknown format. The record is valid only for this call.
    // - It runs inside Drain(). See SharedRingBufferReader::Delegate for
    //   what the chunk bytes can still show.
    // - The delegate can copy them with WriteDump(), and must not destroy the
    //   drainer.
    virtual void OnRingBufferChunkRejected(
        const SharedRingBufferReader::ChunkRejection&) = 0;
  };

  // Shares ownership of |memory|. The mapping and |chunk_size_bytes| must
  // describe a valid ring buffer layout. |delegate|, |clock| and
  // |task_runner| must remain valid until destruction.
  ServiceRingBufferDrainer(std::shared_ptr<SharedMemory> memory,
                           uint32_t chunk_size_bytes,
                           ProducerID,
                           ClientIdentity,
                           Delegate* delegate,
                           tracing_service::Clock* clock,
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

  // The layout and the reader state, for TraceStats.tracing_v2.
  uint32_t chunk_size() const { return ring_buffer_.chunk_size(); }
  uint32_t num_chunks() const { return ring_buffer_.num_chunks(); }
  uint32_t reader_read_pos() const { return reader_.read_pos(); }
  const std::optional<SharedRingBufferReader::ProtocolError>& protocol_error()
      const {
    return reader_.protocol_error();
  }
  SharedRingBufferReader::Stats reader_stats() const {
    return reader_.GetStats();
  }

  // The header, as the producer last wrote it. Untrusted.
  SharedRingBuffer::HeaderSnapshot LoadHeader() const {
    return ring_buffer_.LoadHeaderRelaxed();
  }

  // For diagnostics only. Totals since the drainer was created.
  //
  // Each OnChunkRead() call adds 1 to exactly one admission counter. So,
  // outside Drain():
  //   reader_stats().chunks_read == chunks_admitted + invalid_writer_chunks +
  //       invalid_destination_chunks + the trace_buffer_rejected_* counters
  struct Stats {
    // Drain() calls that ran the reader. A call after a protocol error does
    // not run it.
    uint64_t drain_passes = 0;
    // Passes that consumed no position but ended at the retry limit.
    uint64_t drain_passes_without_progress = 0;
    // Thread CPU time of the reader's passes, including the delegate calls
    // inside them.
    uint64_t drain_cpu_time_ns = 0;

    // Admission outcomes of the chunks that the reader delivered.
    uint64_t chunks_admitted = 0;
    // Fragment payload bytes of the admitted chunks.
    uint64_t admitted_payload_bytes = 0;
    uint64_t invalid_writer_chunks = 0;
    uint64_t invalid_destination_chunks = 0;
    // TraceBufferV2::CopyChunkV2Result, other than kAdmitted.
    uint64_t trace_buffer_rejected_buffer_full = 0;
    uint64_t trace_buffer_rejected_format_conflict = 0;
    uint64_t trace_buffer_rejected_protovm = 0;
    uint64_t trace_buffer_rejected_invalid = 0;
  };
  const Stats& stats() const { return stats_; }

  // The last pass. Times are BOOTTIME, zero before the first pass. Drain()
  // reads the clock once per pass, when the pass starts. The records of the
  // pass use the same time.
  struct LastDrain {
    int64_t time_ns = 0;
    // The last pass that consumed a position.
    int64_t progress_time_ns = 0;
    uint32_t positions_consumed = 0;
    SharedRingBufferReader::ConsumeResult result =
        SharedRingBufferReader::ConsumeResult::kNoData;
  };
  const LastDrain& last_drain() const { return last_drain_; }

  // BOOTTIME of the pass that found the protocol error. Zero if none.
  int64_t protocol_error_time_ns() const { return protocol_error_time_ns_; }

  // The first rejection of each reason, with the BOOTTIME of its pass,
  // indexed by SharedRingBufferReader::ChunkRejection::Reason. Kept for the
  // life of the connection. reader_stats() counts all of them.
  struct RejectionRecord {
    SharedRingBufferReader::ChunkRejection rejection;
    int64_t time_ns = 0;
  };
  using FirstRejections =
      std::array<std::optional<RejectionRecord>,
                 SharedRingBufferReader::ChunkRejection::kNumReasons>;
  const FirstRejections& first_rejections() const { return first_rejections_; }

  // Ring buffer dumps.
  //
  // A dump has the layout, the positions, the header fields, and the state
  // word of each chunk. With |include_chunk_bytes|, it also has chunk bytes.
  // With |only_chunk_pos|, it has only the chunk at that position.
  // - Each word is read once. The copy is not a consistent snapshot.
  // - The loop bounds and the sizes come from the layout validated at attach,
  //   never from shared memory.

  // An upper bound of the serialized size of the complete dump, including
  // the enclosing TracePacket and its service fields.
  size_t DumpSizeBound(bool include_chunk_bytes,
                       std::optional<uint32_t> only_chunk_pos) const;

  // Writes the dump into |dump|, within |max_bytes| of serialized size, as
  // DumpSizeBound() counts it.
  // - If not all chunk bytes fit, copies the chunk at the reader position
  //   first, then the chunks that are not Free, in position order. It lists
  //   their indexes and the omitted count.
  // - Returns false if even the state words do not fit. |dump| is then
  //   incomplete and the caller must discard it.
  bool WriteDump(protos::pbzero::TracingV2RingBufferDump* dump,
                 bool include_chunk_bytes,
                 std::optional<uint32_t> only_chunk_pos,
                 size_t max_bytes) const;

 private:
  friend class test::ServiceRingBufferDrainerTestPeer;

  // SharedRingBufferReader::Delegate implementation.
  // The reader calls these inline during Drain().
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;
  void OnChunkRejected(const SharedRingBufferReader::ChunkRejection&) override;

  // Marks a gap in the sequence for |writer_id| in its destination buffer.
  void RecordWriterLoss(WriterID writer_id);

  // Trusted identity supplied by the endpoint.
  const ProducerID producer_id_;
  const ClientIdentity client_identity_;

  Delegate* const delegate_;
  tracing_service::Clock* const clock_;

  // Declared before |ring_buffer_| and |reader_| so the mapping outlives them.
  std::shared_ptr<SharedMemory> memory_;

  // Constructing |ring_buffer_| leaves the contents of |memory_| unchanged,
  // preserving any data the producer has already written.
  SharedRingBuffer ring_buffer_;
  SharedRingBufferReader reader_;

  bool retry_scheduled_ = false;

  Stats stats_;
  LastDrain last_drain_;
  int64_t protocol_error_time_ns_ = 0;
  FirstRejections first_rejections_;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Declared last to cancel retry tasks before reader or mapping destruction.
  base::WeakRunner weak_runner_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_SERVICE_RING_BUFFER_DRAINER_H_
