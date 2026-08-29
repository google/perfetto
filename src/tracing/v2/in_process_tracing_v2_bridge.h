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

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_runner.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/trace_writer_v2.h"

namespace perfetto::tracing_v2 {

// Temporary producer-side adapter used until traced reads the v2 ring directly.
//
// Current path:
//
//   SDK thread:   TraceWriterV2 -> v2 ring -> NotifyReader()
//   relay thread: ScheduleDrain() -> reader -> reassemble / ProtoGroup rewrite
//                                  -> retained v1 writer -> v1 SMB
//   traced:       v1 SMB -> session TraceBuffer
//
// The bridge:
// - owns the ring memory, view and reader, plus reassembly state and one
//   retained v1 TraceWriter per WriterID;
// - coalesces notifications and forwards canonical packets through v1; and
// - orders control requests and writer retirement after preceding ring data.
//
// Final path:
//
//   producer process                         traced
//   TraceWriterV2 -> shared v2 ring --------> reader -> reassemble / rewrite
//                                                   -> session TraceBuffer
//
// The reader, reassembly and rewriter move to traced. This bridge, its relay,
// retained v1 writers and the extra v1 SMB then disappear.
class InProcessTracingV2Bridge
    : public TraceWriterV2::Delegate,
      public SharedRingBufferReader::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  // 256 KiB, matching TracingService::kDefaultShmSize.
  // TODO(sashwinbalaji): plumb the producer's size hint.
  static constexpr uint32_t kDefaultNumChunks = 1024;
  static constexpr uint32_t kDefaultChunkSize = 256;

  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      base::TaskRunner* relay_task_runner,
      uint32_t num_chunks = kDefaultNumChunks,
      uint32_t chunk_size = kDefaultChunkSize);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge(InProcessTracingV2Bridge&&) = delete;
  InProcessTracingV2Bridge& operator=(InProcessTracingV2Bridge&&) = delete;

  // Retains |v1_writer| and returns a TraceWriterV2. Returns |v1_writer|
  // unchanged if it has no WriterID or teardown has started. Thread-safe.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      std::unique_ptr<TraceWriter> v1_writer,
      BufferID target_buffer,
      BufferExhaustedPolicy);

  // Stops accepting writers and releases retained v1 writers asynchronously.
  // The v1 arbiter must outlive this release. Not called on the relay thread.
  void StartReleasingV1Writers();

  // Returns whether the bridge still retains any v1 TraceWriter. Thread-safe;
  // once false after release starts, remains false.
  bool HasRetainedV1Writers() const;

  // Runs |completion| on the relay after draining current ring data and
  // flushing the affected v1 writers. Thread-safe; completions are FIFO.
  void DrainPendingData(std::function<void()> completion);

 private:
  // Create() installs the custom shared_ptr deleter.
  InProcessTracingV2Bridge(base::PagedMemory ring_memory,
                           uint32_t chunk_size,
                           base::TaskRunner* relay_task_runner);

  struct WriterState {
    std::unique_ptr<TraceWriter> v1_writer;
    BufferID target_buffer = 0;

    // Packet reassembly. Relay thread only.
    std::vector<uint8_t> partial_packet;
    bool expecting_continuation = false;
    bool discarding_packet = false;
    // TracePacket::DataLossReason bits for the next forwarded packet.
    // TODO(sashwinbalaji): add that service-side accounting.
    uint32_t pending_data_loss = 0;
    // The next endpoint-level barrier must flush this writer.
    bool has_unflushed_v1_data = false;
  };

  // Orders control after prior data:
  //   sample write_pos -> drain to it -> flush v1 writers -> completion
  // Barriers run one at a time on the relay thread.
  struct Barrier {
    // Distinguishes late acknowledgements from the current barrier.
    uint64_t id = 0;
    uint32_t drain_target_pos = 0;
    // A v1 WriterID, or 0 for all writers with unflushed data.
    WriterID writer_id = 0;
    size_t pending_flush_acks = 0;
    std::function<void()> completion;
  };

  // TraceWriterV2::Delegate:
  void NotifyReader() override;
  void Flush(WriterID, std::function<void()> callback) override;
  void OnWriterDestroyed(WriterID) override;

  // SharedRingBufferReader::Delegate:
  void OnChunkRead(const SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Thread-safe.
  void ScheduleDrain();
  // Relay thread from here down.
  void DrainOnRelayThread();
  // True once the target is resolved, or abandoned because malformed input
  // stopped the reader. False if a concurrent transition must be retried.
  bool DrainUpTo(uint32_t target_pos);

  void ForwardPacket(WriterState*);

  // Thread-safe. Samples write_pos before posting to the relay.
  void EnqueueBarrier(WriterID, std::function<void()> completion);
  void RunFrontBarrier();
  void OnFrontBarrierFlushAcknowledged(uint64_t barrier_id);
  void CompleteFrontBarrier();
  void FlushV1Writer(WriterID, std::function<void()> callback);

  void RetireV1Writer(WriterID);
  void ReleaseAllV1Writers();

  bool HasPendingRingData() const;
  // Drains before destroying the last reference on the relay thread.
  static void DeleteWhenQuiescent(
      std::shared_ptr<InProcessTracingV2Bridge> owner);

  // Declaration order keeps the mapping alive for the ring and reader.
  base::PagedMemory ring_memory_;
  SharedRingBuffer ring_buffer_;
  SharedRingBufferReader ring_buffer_reader_;

  std::vector<uint8_t> rewritten_packet_;

  // Only the front barrier can be in progress.
  std::deque<Barrier> pending_control_barriers_;
  uint64_t next_barrier_id_ = 1;

  // Guards |accepting_writers_| and the map. WriterState contents are relay-
  // only; indirection keeps them stable across map rehashes.
  mutable std::mutex mutex_;
  // Closed under |mutex_| before releasing the map.
  bool accepting_writers_ = true;
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers_;

  std::atomic<bool> drain_scheduled_{false};

  // Continue draining but discard packets after v1 teardown.
  bool stopped_forwarding_ = false;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Last, so callbacks are invalidated before the state they use.
  base::WeakRunner weak_runner_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
