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

#ifndef SRC_TRACING_SERVICE_TRACING_V2_INGRESS_H_
#define SRC_TRACING_SERVICE_TRACING_V2_INGRESS_H_

#include <stdint.h>

#include <functional>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"

namespace perfetto {

namespace tracing_v2 {
class SharedRingBuffer;
}

// Reads one producer's tracing v2 shared ring in traced and admits its
// fragments into the destination TraceBufferV2. It is the service-side end of
// the direct v2 data path, replacing the in-process bridge:
//
//   Producer                                    traced
//   TraceWriterV2Impl -> v2 shared ring -------> TracingV2Ingress (sole reader)
//                                                   |
//                                       CopyRingChunkFragmentsV2 (raw
//                                       fragments)
//                                                   |
//                                                TraceBufferV2
//                                                   |
//                                        readout: reassemble + canonicalize
//
// Ownership and threading:
// - The endpoint owns the ring mapping and keeps it alive for the ingress'
//   lifetime. The ingress owns the sole SharedRingBufferReader and its scratch.
// - All methods run on the service's task-runner sequence. Drain() copies
//   reader-borrowed fragments into the trace buffer synchronously, before the
//   borrowed view expires; no shared-memory pointer is retained.
// - TraceBufferV2 owns incomplete-packet state, sequence ordering, cursors and
//   loss. The ingress keeps only bounded per-writer routing/loss control state.
class TracingV2Ingress : public tracing_v2::SharedRingBufferReader::Delegate {
 public:
  // Resolves a chunk's target BufferID to the destination v2 trace buffer,
  // applying the producer's current target permissions. Returns nullptr to drop
  // the chunk: no such v2 buffer, or the producer may not write to it.
  using TargetBufferResolver = std::function<TraceBufferV2*(BufferID)>;

  // |ring| and its backing memory must outlive the ingress. |producer_id| and
  // |client_identity| are the authenticated endpoint's trusted identity,
  // stamped onto every admitted fragment.
  TracingV2Ingress(tracing_v2::SharedRingBuffer* ring,
                   ProducerID producer_id,
                   ClientIdentity client_identity,
                   TargetBufferResolver resolver);
  ~TracingV2Ingress() override;

  TracingV2Ingress(const TracingV2Ingress&) = delete;
  TracingV2Ingress& operator=(const TracingV2Ingress&) = delete;

  // Drains newly published ring data into the target buffers, bounded per call.
  // Returns true if the reader may have more work (the caller should schedule
  // another Drain()), false if it caught up or hit a protocol error.
  bool Drain();
  // Reads only reservations before this fixed boundary. Returns true while
  // more work or a deferred read-position publication remains.
  bool DrainUntil(uint32_t end_pos);
  uint32_t write_pos() const;
  void RetireWriter(WriterID);

  // Drains repeatedly until the reader catches up or latches a protocol error,
  // capping the number of passes. The cap keeps teardown finite even if a
  // disconnected producer keeps writing its retained mapping: this chases the
  // write position for at most one full ring, then stops. Used only off the
  // steady-state path (an in-process backpressure unblock and endpoint
  // teardown), never for an ordinary remote notification.
  bool DrainToCompletion();

  // True once the reader latched an unrecoverable ring protocol error. The
  // producer's ring can no longer be trusted; the caller should stop draining
  // it and tear the connection down.
  bool has_protocol_error() const;

  // Heap the ingress holds on top of the ring mapping: the reader's copy-out
  // scratch, the fragment-view scratch and the per-writer routing state. All
  // are bounded (a chunk's payload and the 15-bit writer-id space). The service
  // adds this to its memory guardrail alongside the mapping.
  size_t GetMemoryUsageBytes() const;

 private:
  // Bounds copying and callbacks per Drain() so other service tasks can run
  // between passes over a large ring.
  static constexpr uint32_t kMaxPositionsPerDrain = 256;

  // tracing_v2::SharedRingBufferReader::Delegate:
  void OnChunkRead(
      const tracing_v2::SharedRingBufferReader::ChunkContents&) override;
  void OnDataLoss(WriterID) override;

  // Keep the ordinal through retirement because TBv2 can retain old chunks.
  // The validated 15-bit wire ID bounds this map for the connection's lifetime.
  struct WriterState {
    // Next admission ordinal. Known loss skips one ID before successful
    // storage.
    ChunkID next_chunk_id = 0;
    // Coalesces transport loss, denied routing and rejected storage. Retirement
    // also sets this boundary to separate the next writer incarnation.
    bool loss_pending = false;
  };

  // Returns the mutable state for |writer_id|, creating it on first use.
  WriterState& GetWriterState(WriterID writer_id);

  tracing_v2::SharedRingBuffer* const ring_;
  const ProducerID producer_id_;
  const ClientIdentity client_identity_;
  const TargetBufferResolver resolve_target_;
  tracing_v2::SharedRingBufferReader reader_;
  base::FlatHashMap<WriterID, WriterState> writers_;

  // Reused across chunks to adapt reader fragment views to the trace buffer's
  // input type without a per-chunk allocation.
  std::vector<TraceBufferV2::RingChunkFragment> frag_scratch_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACING_V2_INGRESS_H_
