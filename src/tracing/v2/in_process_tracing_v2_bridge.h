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

#ifndef SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
#define SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_

#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/relay_sequence.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/trace_writer_v2.h"

namespace perfetto::internal {
class TracingMuxerImpl;
}

namespace perfetto::test {
class TracingMuxerImplV2Test;
}

namespace perfetto::tracing_v2 {

class InProcessTracingV2BridgeTestPeer;

// Temporary adapter that lets SDK data sources write through the tracing v2
// ring buffer while traced still only understands the v1 shared memory buffer
// (SMB). It reads the ring buffer in the producer process and re-emits the
// packets through ordinary v1 TraceWriters. NotifyReader() schedules a
// coalesced drain on the relay sequence; the other arrows show packet data
// flow.
//
//   Producer process
//   SDK thread                  Relay sequence
//   TraceWriterV2 --------------->  v2 ring buffer
//         |                                |
//         |                                v
//         +-- NotifyReader() -> SharedRingBufferReader    -+
//                                          |               |
//                                          v               | Temporary
//                                reassemble / rewrite      | adapter
//                                          |               | in producer
//                                          v               |
//                               retained v1 TraceWriter   -+
//                                          |
//                                          v
//                                    shared v1 SMB
//                                          |
//                                          v
//                                       traced
//                                          |
//                                          v
//                                 session TraceBuffer
//
// Once traced reads the ring buffer directly, the relay sequence, retained
// v1 writers and extra v1 SMB go away. Reassembly and rewriting move into the
// service:
//
//   Producer                                    Tracing service
//   TraceWriterV2 -> shared v2 ring buffer ----> ring buffer reader
//                                                   |
//                                                   v
//                                           reassemble / rewrite
//                                                   |
//                                                   v
//                                               TraceBuffer
//
// The bridge owns the ring buffer memory and its reader, the per-writer
// reassembly state and one v1 TraceWriter per v2 WriterID. Writer registration
// is guarded by |mutex_|. Draining, reassembly, v1 forwarding and control
// barriers run one task at a time on the relay sequence.
//
// Lifetime: every TraceWriterV2 holds a shared_ptr to its bridge (the bridge
// is their Delegate), and so does every queued relay task. The last reference
// can go away on any thread. ProducerImpl keeps the endpoint and the arbiter
// backing those v1 writers alive until then (see |dead_services_|).
//
// Shutdown, i.e. after RelaySequence::Close():
// - New internal drains complete inline without draining the ring buffer.
// - New explicit writer flushes discard their callbacks without
//   acknowledgement.
// - New writer destruction requests are dropped. Nobody waits for them.
//   WriterIDs are not reused after shutdown.
// - Tasks queued before the close may still run while the task runner is being
//   destroyed. A drain accepted before the close may not finish if its next
//   batch is posted after the close.
class InProcessTracingV2Bridge
    : public TraceWriterV2::Delegate,
      public SharedRingBufferReader::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  static constexpr uint32_t kDefaultChunkSize = 256;

  // Bounds per-writer scratch for configured writers; matches the v1 page
  // limit. Keep in sync with kMaxTracingV2ChunkSize in
  // src/tracing/service/tracing_service_impl.cc.
  // TODO(sashwinbalaji): Consider a common internal header under src/tracing/
  // if this policy is needed beyond the temporary adapter.
  static constexpr uint32_t kMaxConfiguredChunkSize = 32 * 1024;

  // Largest power-of-two number of chunks that fits in |capacity_bytes|, or 0
  // if not even one fits. Power of two because the ring buffer masks positions
  // by the chunk count. CHECKs that |chunk_size| is nonzero. Capacity covers
  // chunk storage only. The ring buffer header is allocated in addition to it.
  static uint32_t NumChunksForCapacity(size_t capacity_bytes,
                                       uint32_t chunk_size);

  // CHECKs the shared ring buffer ABI layout constraints, a non-null relay,
  // nonzero power-of-two chunk count and at most 32 MiB of chunk storage.
  // Allocates the ring buffer header in addition. The 32 KiB maximum chunk size
  // is producer setup policy (kMaxConfiguredChunkSize), not a factory
  // constraint.
  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      std::shared_ptr<RelaySequence> relay,
      uint32_t num_chunks,
      uint32_t chunk_size);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge(InProcessTracingV2Bridge&&) = delete;
  InProcessTracingV2Bridge& operator=(InProcessTracingV2Bridge&&) = delete;

  // Drains everything written to the ring buffer so far, flushes the v1 writers
  // that received data and then runs |completion| on the relay sequence. With a
  // fully bound v1 arbiter, commits are posted to the muxer sequence before
  // |completion| runs, so anything the completion posts there lands behind
  // them. If the relay is closed, |completion| runs inline without draining.
  // Thread-safe.
  void DrainPendingData(std::function<void()> completion);

 private:
  friend class ::perfetto::internal::TracingMuxerImpl;
  friend class ::perfetto::test::TracingMuxerImplV2Test;
  friend class InProcessTracingV2BridgeTestPeer;

  InProcessTracingV2Bridge(base::PagedMemory ring_memory,
                           uint32_t chunk_size,
                           std::shared_ptr<RelaySequence> relay);

  // Takes ownership of |v1_writer| and returns a TraceWriterV2 with the same
  // WriterID that forwards into it. If |v1_writer| has no usable WriterID, it
  // is returned as is. Called by TracingV2Connection. Thread-safe.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      std::unique_ptr<TraceWriter> v1_writer,
      BufferID target_buffer,
      BufferExhaustedPolicy);

  struct WriterState {
    std::unique_ptr<TraceWriter> v1_writer;
    BufferID target_buffer = 0;

    // Packet reassembly. Relay sequence only.
    std::vector<uint8_t> partial_packet;
    bool expecting_continuation = false;
    bool discarding_packet = false;
    // TracePacket::DataLossReason bits to set on the next packet forwarded to
    // v1. If v1 then drops that packet the reason is lost and the trace only
    // shows v1's own DATA_LOSS_SMB_FULL.
    uint32_t pending_data_loss = 0;
    // Set after forwarding a packet, cleared when the v1 writer is flushed.
    bool has_unflushed_v1_data = false;
  };

  // Work to perform after the reader reaches a barrier's ring buffer position.
  enum class BarrierType {
    // Flush one v1 writer. Completion runs with its acknowledgement.
    kFlushWriter,
    // Flush every v1 writer that received data since its last flush.
    kFlushDirtyWriters,
    // Forward a writer's remaining data, then destroy its retained v1 writer.
    kDestroyWriter,
  };

  // A control operation (flush, drain, writer destruction) that must run after
  // all the ring buffer data written before it was requested:
  //   sample write_pos -> drain up to it -> flush v1 writer(s) -> completion
  // Barriers run one at a time, in request order, on the relay sequence.
  struct Barrier {
    uint32_t drain_target_pos = 0;
    BarrierType type = BarrierType::kFlushDirtyWriters;
    // The v1 WriterID for flush/destruction, or 0 for kFlushDirtyWriters.
    WriterID writer_id = 0;
    // Service ACK callback for kFlushWriter, relay completion for
    // kFlushDirtyWriters, empty for kDestroyWriter.
    std::function<void()> completion;
  };

  // TraceWriterV2::Delegate: SDK writer threads. Calls may overlap.
  SharedRingBuffer& ring_buffer() override;
  // Also called by the relay to reschedule a bounded drain.
  void NotifyReader() override;
  void Flush(WriterID, std::function<void()> callback) override;
  void OnWriterDestroyed(WriterID) override;

  // SharedRingBufferReader::Delegate and packet processing: relay sequence.
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // The returned pointer stays valid without holding |mutex_|: entries are
  // heap allocated, so a concurrent insert cannot move them, and only the
  // relay sequence ever removes them.
  WriterState* FindWriterState(WriterID);
  static void DiscardCurrentPacket(WriterState*);
  void ForwardPacket(WriterState*);

  // Barrier submission: thread-safe.
  // Samples write_pos and posts a barrier for it. Returns false if the relay
  // is closed. Takes ownership of |completion| even on rejection. Callers that
  // need to invoke the callback after rejection must pass a copy.
  bool EnqueueBarrier(BarrierType, WriterID, std::function<void()> completion);

  // Barrier execution: relay sequence only.
  // Drains one bounded batch for the current control barrier. Returns true when
  // |target_pos| is reached or the reader cannot continue.
  bool DrainBarrierBatch(uint32_t target_pos);

  void RunFrontBarrier();

  // Order matters: the ring buffer and the reader point into |ring_memory_|.
  base::PagedMemory ring_memory_;
  SharedRingBuffer ring_buffer_;
  SharedRingBufferReader ring_buffer_reader_;

  std::vector<uint8_t> rewritten_packet_;

  // Enqueue tasks append. RunFrontBarrier drains and removes the front.
  std::deque<Barrier> pending_control_barriers_;

  // Guards |writers_| only, as any SDK thread can insert into it. The
  // WriterState contents are relay-sequence only. The unique_ptr keeps them
  // stable across rehashes.
  std::mutex mutex_;
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers_;

  std::atomic<bool> drain_scheduled_{false};

  const std::shared_ptr<RelaySequence> relay_;

  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
