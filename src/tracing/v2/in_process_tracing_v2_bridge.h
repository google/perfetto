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
#include "src/tracing/v2/trace_writer_v2_impl.h"

namespace perfetto::internal {
class TracingMuxerImpl;
}

namespace perfetto::test {
class TracingMuxerImplV2Test;
}

namespace perfetto::tracing_v2 {

class InProcessTracingV2BridgeTestPeer;

// Temporary adapter that lets SDK data sources write through the tracing v2
// ring buffer while traced reads the v1 shared memory buffer (SMB). The bridge
// reads the ring buffer in the producer process and forwards complete packets
// through v1 TraceWriters. NotifyReader() schedules a drain on the relay
// sequence. The other arrows show packet data flow.
//
//   Producer process
//   SDK thread                  Relay sequence
//   TraceWriterV2Impl ----------->  v2 ring buffer
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
//   TraceWriterV2Impl -> v2 ring buffer -------> ring buffer reader
//                                                   |
//                                                   v
//                                           reassemble / rewrite
//                                                   |
//                                                   v
//                                               TraceBuffer
//
// The bridge owns the ring buffer memory, its reader, and one WriterState per
// v2 WriterID. Each WriterState holds packet reassembly state and a v1 writer.
// SDK threads register writers under |mutex_|. The relay reads the ring buffer,
// reassembles packets, forwards them to v1, and processes barriers.
// A barrier delays a control operation until the reader reaches a sampled
// ring buffer position. See Barrier below.
//
// Each TraceWriterV2Impl retains the bridge as its Delegate. Queued relay tasks
// also hold shared references, so destruction can happen on any thread.
// ProducerImpl keeps the endpoint and arbiter alive while the retained v1
// writers need them (see |dead_services_|).
//
// After RelaySequence::Close():
// - New internal drains complete inline without draining the ring buffer.
// - New explicit writer flushes discard their callbacks without
//   acknowledgement.
// - New writer destruction requests are dropped. The bridge releases retained
//   writers when it dies. WriterIDs are not reused after shutdown.
// - Tasks queued before the close can still run during task runner destruction.
//   A drain can need several tasks. If Close() rejects its next batch, the
//   drain remains incomplete even though its first task was accepted.
class InProcessTracingV2Bridge
    : public TraceWriterV2Impl::Delegate,
      public SharedRingBufferReader::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  static constexpr uint32_t kDefaultChunkSize = 256;

  // Bounds per-writer scratch for configured writers. Matches the v1 page
  // limit. Keep in sync with kMaxTracingV2ChunkSize in
  // src/tracing/service/tracing_service_impl.cc.
  // TODO(sashwinbalaji): Consider a common internal header under src/tracing/
  // if this policy is needed beyond the temporary adapter.
  static constexpr uint32_t kMaxConfiguredChunkSize = 32 * 1024;

  // Returns the largest power-of-two chunk count that fits, or 0 if fewer than
  // two fit. The ring buffer uses this count to mask positions into indices.
  // |chunk_size| must be nonzero. Capacity excludes the ring buffer header.
  static uint32_t NumChunksForCapacity(size_t capacity_bytes,
                                       uint32_t chunk_size);

  // Requires a non-null relay and a layout that satisfies SharedRingBuffer's
  // ABI checks. The chunk count must be a power of two and at least two.
  // Chunk storage must not exceed 32 MiB. Invalid arguments fail a CHECK.
  // Allocates the ring buffer header in addition to that storage.
  // Producer setup enforces kMaxConfiguredChunkSize (32 KiB). This factory
  // accepts larger chunks if the layout and total storage satisfy its checks.
  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      std::shared_ptr<RelaySequence> relay,
      uint32_t num_chunks,
      uint32_t chunk_size);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge(InProcessTracingV2Bridge&&) = delete;
  InProcessTracingV2Bridge& operator=(InProcessTracingV2Bridge&&) = delete;

  // Samples the ring buffer's write position, then queues a drain up to it.
  // The relay forwards complete packets and flushes the v1 writers that
  // received them. It then runs |completion| on the relay sequence.
  // Finish packets before requesting the drain to include them.
  //
  // With a fully bound v1 arbiter, commits are posted to the muxer before
  // |completion| runs. Work it posts to the muxer follows those commits.
  // Completion does not wait for the service to acknowledge the commits.
  // If the relay is closed, |completion| runs inline without draining.
  // Thread-safe. See the class comment for interrupted drains during shutdown.
  void DrainPendingData(std::function<void()> completion);

 private:
  friend class ::perfetto::internal::TracingMuxerImpl;
  friend class ::perfetto::test::TracingMuxerImplV2Test;
  friend class InProcessTracingV2BridgeTestPeer;

  InProcessTracingV2Bridge(base::PagedMemory ring_memory,
                           uint32_t chunk_size,
                           std::shared_ptr<RelaySequence> relay);

  // Retains |v1_writer| as the destination for a new TraceWriterV2Impl with the
  // same WriterID. The relay forwards that v2 writer's complete packets to it.
  // If |v1_writer| has no usable WriterID, returns it unchanged.
  // Called by TracingV2Connection. Thread-safe.
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
    // Loss bits for the next complete packet's previous_packet_dropped field.
    // ForwardPacket() clears them after it writes the field to the v1 writer.
    // If v1 drops that packet, these bits are lost.
    // V1 reports DATA_LOSS_SMB_FULL on recovery.
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

  // A control operation (flush, drain, writer destruction) ordered after
  // earlier packet publication:
  //   sample write_pos -> read published fragments -> flush v1 -> completion
  // The relay processes barriers in the order their enqueue tasks run.
  // It submits each v1 flush before starting the next barrier, but does not
  // wait for that flush's service acknowledgement.
  struct Barrier {
    uint32_t drain_target_pos = 0;
    BarrierType type = BarrierType::kFlushDirtyWriters;
    // The v1 WriterID for flush/destruction, or 0 for kFlushDirtyWriters.
    WriterID writer_id = 0;
    // Service ACK callback for kFlushWriter, relay completion for
    // kFlushDirtyWriters, empty for kDestroyWriter.
    std::function<void()> completion;
  };

  // TraceWriterV2Impl::Delegate: SDK writer threads. Calls may overlap.
  // Also called by the relay to reschedule a bounded drain.
  void NotifyReader() override;
  void Flush(WriterID, std::function<void()> callback) override;
  void OnWriterDestroyed(WriterID) override;

  // SharedRingBufferReader::Delegate and packet processing: relay sequence.
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Called only on the relay sequence. The returned pointer remains valid
  // after unlocking: registration does not move WriterState objects, and only
  // the relay removes them.
  WriterState* FindWriterState(WriterID);
  static void DiscardCurrentPacket(WriterState*, uint32_t reason);
  void ForwardPacket(WriterState*);

  // Samples write_pos and queues a barrier. Thread-safe.
  // Returns false if the relay is closed and drops |completion| in that case.
  // Pass a copy if the caller needs to invoke it after rejection.
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

  // Relay sequence only. Enqueue tasks append barriers. RunFrontBarrier()
  // drains to the front barrier's target, then removes and executes it.
  std::deque<Barrier> pending_control_barriers_;

  // Guards |writers_| because SDK threads register writers while the relay
  // reads and removes entries. After registration, only the relay accesses
  // WriterState contents. The unique_ptr keeps their addresses stable across
  // rehashes.
  std::mutex mutex_;
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers_;

  std::atomic<bool> drain_scheduled_{false};

  const std::shared_ptr<RelaySequence> relay_;

  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
