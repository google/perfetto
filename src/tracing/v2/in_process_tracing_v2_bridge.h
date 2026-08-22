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
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/weak_runner.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/chunk_reader.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/trace_writer_v2.h"

namespace perfetto {
namespace tracing_v2 {

// Temporary producer-side v2-to-v1 adapter. SDK threads write to one v2
// ring; |relay_task_runner| drains it and forwards canonical packets through
// ordinary v1 TraceWriters. One downstream writer is retained per v2 WriterID
// so packet sequence and incremental-state identity stay unchanged. The extra
// writer and serialization hop are validation scaffolding, not representative
// of the final transport cost. Delete this class when the service consumes v2
// chunks and owns v2 flush and retirement acknowledgements directly.
//
// The relay is consumer-only: it stands in for the traced-side reader, so it
// must never write into the ring it drains. It can, however, depend on the
// muxer: forwarding into a downstream writer whose shared memory buffer is
// full needs the muxer to issue the arbiter's pending commits, because the
// arbiter refuses to commit synchronously off its own sequence. The muxer must
// therefore never wait for the relay. See DrainThrough().
//
// |relay_task_runner| is owned by the muxer and outlives every bridge and v2
// writer that can post to it.
class InProcessTracingV2Bridge
    : public TraceWriterV2::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  // An initial validation size, not a protocol default.
  // TODO(sashwinbalaji): select ring and chunk sizing from measurements before
  // enabling the path broadly or freezing the shared-memory ABI.
  static constexpr uint32_t kDefaultNumChunks = 1024;

  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      base::TaskRunner* relay_task_runner,
      uint32_t num_chunks = kDefaultNumChunks);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;

  // Thread-safe. The caller creates |downstream| on the decorated endpoint;
  // its WriterID is reused verbatim in the v2 chunk header.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      std::unique_ptr<TraceWriter> downstream,
      BufferID target_buffer,
      BufferExhaustedPolicy);

  // The ring's next logical reservation position. Sampled on the muxer
  // sequence to fence a control operation against the data plane.
  uint32_t write_pos() const;

  // Runs |completion| on the relay sequence once everything published before
  // |watermark| has been forwarded and the downstream writers it touched have
  // been flushed.
  //
  // A v1 writer puts its packets in the shared memory buffer as it goes, so the
  // producer can promise the service that they are there. A v2 packet is only
  // in the ring until the relay moves it, and nothing else orders that against
  // the promise; this is what does.
  //
  // Deliberately asynchronous. The caller must never wait for the relay:
  // forwarding into a stalling downstream writer needs the muxer to issue the
  // arbiter's pending commits, so a muxer that waited here could not free the
  // buffer the relay is waiting on.
  void DrainThrough(uint32_t watermark, std::function<void()> completion);

  // Whether a drain task is posted and has not started yet. Exposed so a test
  // can assert that the flag is already clear while the ring is being read,
  // which is what lets a commit arriving mid-drain schedule a fresh pass.
  bool drain_scheduled_for_testing() const {
    return drain_scheduled_.load(std::memory_order_relaxed);
  }

 private:
  InProcessTracingV2Bridge(base::TaskRunner* relay_task_runner,
                           uint32_t num_chunks);

  struct WriterState {
    std::unique_ptr<TraceWriter> trace_writer;
    BufferID target_buffer = 0;
  };

  struct PendingOperation {
    enum class Type {
      kFlush,
      kRetire,
    };

    Type type = Type::kFlush;
    WriterID writer_id = 0;
    uint32_t pos = 0;
    std::function<void()> callback;
  };

  void OnPacketsCommitted() override;
  void OnWriterFlush(WriterID, uint32_t pos, std::function<void()>) override;
  void OnWriterDestroyed(WriterID, uint32_t pos) override;

  void ScheduleDrain();
  void DrainOnTaskRunner();
  void ForwardPacket(const ChunkReader::Packet&);
  void FlushTouchedWriters(const std::vector<WriterID>& already_flushed);
  // Returns true if any operation is still waiting for the relay to reach its
  // position, i.e. this bridge is not quiescent yet.
  bool ProcessPendingOperations(std::vector<WriterID>* flushed_writers);
  bool HasOutstandingWork();
  void DeleteWhenQuiescent(uint32_t attempt);

  SharedRingBuffer ring_buffer_;
  ChunkReader chunk_reader_;

  // Writers that received a packet in the current drain pass, so the batch can
  // be committed with one Flush() each instead of one per packet. Relay-only;
  // a flat vector because a pass touches a handful of writers at most.
  std::vector<WriterID> touched_writers_;

  // CreateTraceWriter runs on arbitrary SDK threads. Only insertion and the
  // operation queue are cross-thread; downstream writers are used and erased
  // exclusively on the relay sequence.
  std::mutex mutex_;
  base::FlatHashMap<WriterID, WriterState> writers_;
  std::vector<PendingOperation> pending_operations_;
  std::atomic<bool> drain_scheduled_{false};

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Last member: queued callbacks are invalidated before referenced state is
  // destroyed.
  base::WeakRunner weak_runner_;
};

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
