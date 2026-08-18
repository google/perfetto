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

namespace perfetto::tracing_v2 {

// Temporary producer-side v2-to-v1 adapter, so that the ring can be exercised
// in a real tracing session before traced can read one.
//
// SDK threads write packets into one producer-local v2 ring.
// |relay_task_runner| drains it, reassembles packets from their fragments,
// rewrites the private nested-message framing to ordinary length-delimited
// protobuf, and forwards each packet through an ordinary v1 TraceWriter. One
// downstream writer is retained per v2 WriterID, so the packet sequence and the
// incremental-state identity a consumer sees are the same as they would be
// without the detour.
//
// The extra writer and the serialization hop are validation scaffolding, not
// representative of the final transport cost. Delete this class when the
// service consumes v2 chunks directly.
//
// The relay is consumer-only: it stands in for the traced-side reader, so it
// must never write into the ring it drains. It can, however, depend on the
// muxer: forwarding into a downstream writer whose shared memory buffer is full
// needs the muxer to issue the arbiter's pending commits, because the arbiter
// refuses to commit synchronously off its own sequence. The muxer must
// therefore never block waiting for the relay.
//
// |relay_task_runner| is owned by the muxer and outlives every bridge and v2
// writer that can post to it.
class InProcessTracingV2Bridge
    : public TraceWriterV2::Delegate,
      public ChunkReader::Delegate,
      public std::enable_shared_from_this<InProcessTracingV2Bridge> {
 public:
  // 1024 x 256 bytes is 256 KiB, the same footprint as the default v1 shared
  // memory buffer, so enabling the path does not change a producer's memory
  // profile while it is being validated.
  // TODO(sashwinbalaji): choose the ring and chunk geometry from measurements
  // before enabling this broadly or freezing the shared-memory ABI.
  static constexpr uint32_t kDefaultNumChunks = 1024;
  static constexpr uint32_t kDefaultChunkSize = 256;

  // Returns nullptr if the ring could not be allocated, in which case the
  // caller keeps using v1.
  static std::shared_ptr<InProcessTracingV2Bridge> Create(
      base::TaskRunner* relay_task_runner,
      uint32_t num_chunks = kDefaultNumChunks,
      uint32_t chunk_size = kDefaultChunkSize);

  ~InProcessTracingV2Bridge() override;

  InProcessTracingV2Bridge(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge& operator=(const InProcessTracingV2Bridge&) = delete;
  InProcessTracingV2Bridge(InProcessTracingV2Bridge&&) = delete;
  InProcessTracingV2Bridge& operator=(InProcessTracingV2Bridge&&) = delete;

  // Thread-safe; called from arbitrary SDK threads. |downstream| is created by
  // the caller on the decorated endpoint and its WriterID is reused verbatim in
  // the v2 chunk header, which is what keeps the sequence identity. If the
  // WriterID cannot be used, |downstream| is handed straight back so the caller
  // transparently stays on v1.
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      std::unique_ptr<TraceWriter> downstream,
      BufferID target_buffer,
      BufferExhaustedPolicy);

  // Phase one of teardown, and it returns immediately.
  //
  // Whoever owns those writers' shared-memory arbiter has to call this before
  // letting it go: a v1 TraceWriter releases its WriterID back to its arbiter
  // when it is destroyed, and Step 1 deliberately keeps the writers alive until
  // this bridge is torn down, which can be later and on another thread.
  //
  // By the time it returns, no further writer will be accepted - that part is
  // synchronous, so the caller can rely on the set of retained writers being
  // closed. Handing them back happens on the relay sequence afterwards, because
  // that is the only sequence allowed to touch a retained writer.
  //
  // It deliberately does not wait. A drain already in flight can be inside a
  // downstream writer waiting for the muxer to issue the arbiter's pending
  // commits, so a muxer that waited for the relay here would be waiting for
  // itself. The caller must instead keep the arbiter alive until
  // holds_downstream_writers() is false; see TracingV2ProducerEndpoint.
  void StartReleasingDownstreamWriters();

  // Whether the bridge is still holding writers borrowed from the caller. False
  // before one was ever created and, once the release has run, false for good.
  // Thread-safe: whoever owns the arbiter those writers belong to has to be
  // able to ask from its own sequence.
  bool holds_downstream_writers() const;

  // Drains until everything the ring holds at the moment the relay picks this
  // up has been forwarded and the downstream writers it touched have been
  // committed, then runs |completion| on the relay sequence.
  //
  // Test-only. Production code must not synchronize its control plane on the
  // relay: see the ordering limitation documented on
  // TracingV2ProducerEndpoint. Doing it properly needs a position-tracking
  // protocol that Step 1 deliberately does not have.
  void DrainToQuiescenceForTesting(std::function<void()> completion);

  // Whether a drain task is posted and has not started yet. Exposed so a test
  // can assert the flag is already clear while the ring is being read, which is
  // what lets a commit arriving mid-drain schedule a fresh pass.
  bool drain_scheduled_for_testing() const {
    return drain_scheduled_.load(std::memory_order_relaxed);
  }

  // Diagnostics, sampled by tests. Relay sequence only.
  uint64_t num_packets_forwarded_for_testing() const {
    return num_packets_forwarded_;
  }
  uint64_t num_malformed_packets_for_testing() const {
    return num_malformed_packets_;
  }

 private:
  InProcessTracingV2Bridge(std::unique_ptr<SharedRingBuffer>,
                           base::TaskRunner* relay_task_runner);

  struct WriterState {
    std::unique_ptr<TraceWriter> trace_writer;
    BufferID target_buffer = 0;

    // Reassembly state, relay sequence only. A packet that ran out of room in
    // its chunk is continued in the writer's next one; these hold the pieces
    // until the last of them arrives.
    std::vector<uint8_t> pending_packet;
    // The previous chunk from this writer ended with "continues on next".
    bool expecting_continuation = false;
    // The beginning of |pending_packet| was lost, so the rest of it goes too.
    bool pending_packet_broken = false;
    // Loss to report on the next packet this writer's downstream emits, as a
    // TracePacket::DataLossReason bitmask.
    uint32_t pending_data_loss = 0;
  };

  // TraceWriterV2::Delegate:
  void OnPacketsCommitted() override;

  // ChunkReader::Delegate:
  void OnChunkRead(const ChunkReader::ChunkContents&) override;

  void ReleaseDownstreamWritersOnRelay();
  void ScheduleDrain();
  void DrainOnRelayThread();
  void EmitPacket(WriterID, WriterState*);
  void FlushTouchedWriters();
  bool HasPendingRingData() const;
  // Drains what the ring still holds and then drops |owner|, the last reference
  // to the bridge, on the relay sequence. Static and by value because the point
  // is that the work owns the object: a queued callback that is destroyed
  // rather than run destroys the bridge instead of leaking it.
  static void DeleteWhenQuiescent(
      std::shared_ptr<InProcessTracingV2Bridge> owner,
      uint32_t attempt);

  const std::unique_ptr<SharedRingBuffer> ring_buffer_;
  ChunkReader chunk_reader_;

  // Scratch for the private-to-canonical rewrite, reused across packets so a
  // drain pass does not allocate per packet.
  std::vector<uint8_t> canonical_packet_;

  // Writers that received a packet in this drain pass, so the batch is
  // committed with one Flush() each instead of one per packet. Relay-only; a
  // flat vector because a pass touches a handful of writers at most.
  std::vector<WriterID> touched_writers_;

  // CreateTraceWriter() runs on arbitrary SDK threads, so insertion is
  // cross-thread. Everything else about a WriterState - the downstream writer
  // and the reassembly fields - is touched only on the relay sequence.
  mutable std::mutex mutex_;
  // Whether a writer created now would still be released at teardown. It is
  // under the same lock as |writers_| so that accepting a writer and closing
  // the set have one order: a creator that finds this true has inserted before
  // the release moves the map, and one that finds it false gets its v1 writer
  // handed straight back. Without that, a writer inserted after the move would
  // never be released, would hold a WriterID in an arbiter nobody can shut
  // down, and would be attached to a relay that has stopped forwarding.
  bool accepting_writers_ = true;
  // The value is held by pointer so that a WriterState stays put when an
  // insertion from another SDK thread rehashes the map: the relay looks a
  // state up under the lock and then works on it without holding one, because
  // forwarding a packet runs downstream writer machinery.
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers_;

  std::atomic<bool> drain_scheduled_{false};

  // Set once the release has run on the relay: there is nowhere left to forward
  // to, so the relay stops reading. Relay sequence only.
  bool stopped_forwarding_ = false;

  uint64_t num_packets_forwarded_ = 0;
  uint64_t num_malformed_packets_ = 0;

  PERFETTO_THREAD_CHECKER(thread_checker_)

  // Last member: queued callbacks are invalidated before any state they might
  // reference is destroyed.
  //
  // PostTask() runs on arbitrary SDK threads - OnPacketsCommitted() reaches it
  // from whichever thread wrote a packet - and WeakRunner requires the caller
  // to keep *this* alive for the duration of the call. What does that is the
  // shared_ptr every v2 writer holds: the only threads that post are the ones
  // running such a writer, and a writer is destroyed before it drops its
  // reference.
  base::WeakRunner weak_runner_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_IN_PROCESS_TRACING_V2_BRIDGE_H_
