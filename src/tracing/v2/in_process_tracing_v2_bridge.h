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
// ring while traced still only understands the v1 shared memory buffer (SMB).
// It reads the ring in the producer process and re-emits the packets through
// ordinary v1 TraceWriters:
//
//   producer process
//   +----------------------------------------------------------------------+
//   | SDK thread                         relay sequence                    |
//   |                                                                      |
//   | TraceWriterV2 -- writes --> v2 ring <-- drains -- RingBufferReader   |
//   |      |                                                   |           |
//   |      +-- NotifyReader() -------------------------------> |           |
//   |                                                          v           |
//   |                                                 reassemble / rewrite |
//   |                                                          |           |
//   |                                                          v           |
//   |                                                 retained v1 writer   |
//   +----------------------------------------------------------|-----------+
//                                                              v
//                                                        shared v1 SMB
//                                                              |
//                                                              v
//                                                           traced
//                                                              |
//                                                              v
//                                                       session TraceBuffer
//
// Once traced reads the ring directly, the reader, the reassembly and the
// rewriter move there. This class, the relay sequence and the second copy of
// the data in the SMB go away:
//
//   producer process                              traced
//   +--------------------------+                  +------------------------+
//   | TraceWriterV2 -> v2 ring |----------------->| RingBufferReader       |
//   +--------------------------+                  |          |             |
//                                                 |          v             |
//                                                 | reassemble / rewrite   |
//                                                 |          |             |
//                                                 |          v             |
//                                                 | session TraceBuffer    |
//                                                 +------------------------+
//
// The bridge owns the ring memory and its reader, the per-writer reassembly
// state and one v1 TraceWriter per v2 WriterID. Writer registration is guarded
// by |mutex_|. Draining, reassembly, v1 forwarding, barriers and retirement
// run one task at a time on the relay sequence.
//
// Lifetime: every TraceWriterV2 holds a shared_ptr to its bridge (the bridge
// is their Delegate), and so does every queued relay task. The last reference
// can go away on any thread. ProducerImpl keeps the endpoint and the arbiter
// backing those v1 writers alive until then (see |dead_services_|).
//
// Shutdown, i.e. after RelaySequence::Close():
// - New drains complete inline without draining the ring.
// - New writer retirements are dropped. Nobody waits for them and WriterIDs
//   are not reused after shutdown.
// - Tasks queued before the close may still run while the task runner is being
//   destroyed. A drain accepted before the close may not finish if its next
//   batch is posted after the close.
class InProcessTracingV2Bridge
    : public TraceWriterV2::Delegate,
      public SharedRingBufferReader::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  static constexpr uint32_t kDefaultChunkSize = 256;

  // Each writer allocates chunk-sized scratch buffers, so cap the chunk size
  // independently of the ring size. 32 KB matches the max v1 SMB page size.
  static constexpr uint32_t kMaxChunkSize = 32 * 1024;

  // Largest power-of-two number of chunks that fits in |capacity_bytes|, or 0
  // if not even one fits. Power of two because the ring masks positions by
  // the chunk count.
  static uint32_t NumChunksForCapacity(size_t capacity_bytes,
                                       uint32_t chunk_size);

  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      std::shared_ptr<RelaySequence> relay,
      uint32_t num_chunks,
      uint32_t chunk_size);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge(InProcessTracingV2Bridge&&) = delete;
  InProcessTracingV2Bridge& operator=(InProcessTracingV2Bridge&&) = delete;

  // Drains everything written to the ring so far, flushes the v1 writers that
  // received data and then runs |completion| on the relay sequence. With a
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

  // Work to perform after the reader reaches a barrier's ring position.
  enum class BarrierType {
    // Flush one v1 writer. Completion runs with its acknowledgement.
    kFlushWriter,
    // Flush every v1 writer that received data since its last flush.
    kFlushDirtyWriters,
    // Forward a writer's remaining data, then release its v1 writer.
    kRetireWriter,
  };

  // A control operation (flush, drain, writer retirement) that must run after
  // all the ring data written before it was requested:
  //   sample write_pos -> drain up to it -> flush v1 writer(s) -> completion
  // Barriers run one at a time, in request order, on the relay sequence.
  struct Barrier {
    // Lets a continuation task check that the barrier it was posted for is
    // still the front one.
    uint64_t id = 0;
    uint32_t drain_target_pos = 0;
    BarrierType type = BarrierType::kFlushDirtyWriters;
    // The v1 WriterID for flush/retirement, or 0 for kFlushDirtyWriters.
    WriterID writer_id = 0;
    std::function<void()> completion;
  };

  // --- Ring data path. ---
  SharedRingBuffer& ring_buffer() override;
  void NotifyReader() override;
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // The returned pointer stays valid without holding |mutex_|: entries are
  // heap allocated, so a concurrent insert cannot move them, and only the
  // relay sequence ever removes them.
  WriterState* FindWriterState(WriterID);
  static void DiscardCurrentPacket(WriterState*);
  static void AddDataLoss(WriterState*, uint32_t reason = 0);
  void ForwardPacket(WriterState*);

  // --- Control barriers. ---
  void Flush(WriterID, std::function<void()> callback) override;
  void OnWriterDestroyed(WriterID) override;

  // Samples write_pos and posts a barrier for it. Returns false if the relay
  // is closed. |completion| is consumed either way, so callers that want to
  // run it inline on rejection must keep their own copy. Thread-safe.
  bool EnqueueBarrier(BarrierType, WriterID, std::function<void()> completion);

  // --- Relay-side barrier processing. ---
  // Drains one bounded batch for the current control barrier. Returns true when
  // |target_pos| is reached or the reader cannot continue.
  bool DrainBarrierBatch(uint32_t target_pos);

  void RunFrontBarrier();

  // Order matters: the ring and the reader point into |ring_memory_|.
  base::PagedMemory ring_memory_;
  SharedRingBuffer ring_buffer_;
  SharedRingBufferReader ring_buffer_reader_;

  std::vector<uint8_t> rewritten_packet_;

  // Only the front barrier can be in progress.
  std::deque<Barrier> pending_control_barriers_;
  uint64_t next_barrier_id_ = 1;

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
