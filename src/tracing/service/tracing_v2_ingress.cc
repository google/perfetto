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

#include "src/tracing/service/tracing_v2_ingress.h"

#include <utility>

#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto {

TracingV2Ingress::TracingV2Ingress(tracing_v2::SharedRingBuffer* ring,
                                   ProducerID producer_id,
                                   ClientIdentity client_identity,
                                   TargetBufferResolver resolver)
    : ring_(ring),
      producer_id_(producer_id),
      client_identity_(std::move(client_identity)),
      resolve_target_(std::move(resolver)),
      reader_(ring, this) {
  PERFETTO_DCHECK(ring_);
  PERFETTO_DCHECK(resolve_target_);
}

TracingV2Ingress::~TracingV2Ingress() = default;

bool TracingV2Ingress::Drain() {
  return reader_.Drain(kMaxPositionsPerDrain).needs_another_drain();
}

void TracingV2Ingress::DrainToCompletion() {
  // Cap the passes so a producer that keeps writing its retained mapping after
  // disconnect cannot make this loop unbounded. The largest ring the service
  // accepts (the 8 MiB adoption budget with the 256-byte minimum chunk) holds
  // fewer than 32768 chunks, about 128 passes of kMaxPositionsPerDrain. 512
  // passes drain a healthy full ring several times over, then give up.
  constexpr uint32_t kMaxCompletionPasses = 512;
  for (uint32_t pass = 0; pass < kMaxCompletionPasses; pass++) {
    if (has_protocol_error() || !Drain())
      return;
  }
}

bool TracingV2Ingress::has_protocol_error() const {
  return reader_.has_protocol_error();
}

size_t TracingV2Ingress::GetMemoryUsageBytes() const {
  return reader_.GetMemoryUsageBytes() +
         frag_scratch_.capacity() * sizeof(TraceBufferV2::RingChunkFragment) +
         writers_.size() * (sizeof(WriterID) + sizeof(WriterState));
}

TracingV2Ingress::WriterState& TracingV2Ingress::GetWriterState(
    WriterID writer_id) {
  return writers_[writer_id];
}

void TracingV2Ingress::OnChunkRead(
    const tracing_v2::SharedRingBufferReader::ChunkContents& chunk) {
  // The writer id comes from the untrusted ring state word. Validate it against
  // the public range at this receipt boundary, before any per-writer state
  // lookup, so ingress and storage use one identity. An out-of-range id cannot
  // be attributed to a valid writer, so drop the chunk.
  if (PERFETTO_UNLIKELY(chunk.writer_id == 0 ||
                        chunk.writer_id > kMaxWriterID)) {
    return;
  }
  WriterState& ws = GetWriterState(chunk.writer_id);

  // Resolve the destination under the producer's current permissions. A revoked
  // or unknown target drops the chunk; keep the loss pending so it lands on the
  // next packet the writer stores to a permitted buffer.
  TraceBufferV2* target = resolve_target_(chunk.target_buffer);
  if (PERFETTO_UNLIKELY(!target)) {
    ws.loss_pending = true;
    return;
  }

  // Adapt the reader's fragment views (into its own scratch, valid only for
  // this call) to the trace buffer's input type, then admit them. The
  // producer's raw ProtoGroup bytes are stored verbatim; reassembly and
  // canonicalization happen at readout.
  frag_scratch_.clear();
  frag_scratch_.reserve(chunk.num_fragments);
  for (uint32_t i = 0; i < chunk.num_fragments; i++) {
    frag_scratch_.push_back({chunk.fragments[i].data, chunk.fragments[i].size});
  }
  const bool cont_from_prev =
      chunk.payload_flags & tracing_v2::kFlagContinuesFromPrevChunk;
  const bool cont_on_next =
      chunk.payload_flags & tracing_v2::kFlagContinuesOnNextChunk;
  const TraceBufferV2::AdmitResult result = target->CopyRingChunkFragmentsV2(
      producer_id_, client_identity_, chunk.writer_id, ws.next_chunk_id,
      frag_scratch_.data(), frag_scratch_.size(), cont_from_prev, cont_on_next,
      ws.loss_pending);

  if (result == TraceBufferV2::AdmitResult::kStored) {
    // Stored: this chunk carries any pending loss now, and takes the next id.
    ws.next_chunk_id++;
    ws.loss_pending = false;
  } else {
    // Dropped (discard-sealed, or larger than the buffer): the pending loss was
    // not attached to a stored chunk, so keep it for the next one. The id is
    // not consumed, so stored chunk ids stay contiguous.
    ws.loss_pending = true;
  }
}

void TracingV2Ingress::OnDataLoss(WriterID writer_id) {
  if (PERFETTO_UNLIKELY(writer_id == 0 || writer_id > kMaxWriterID))
    return;  // Cannot attribute a loss to an out-of-range writer id.
  // The next stored chunk for this writer carries the loss, so it surfaces on
  // that chunk's first packet and breaks any packet reassembling across it.
  GetWriterState(writer_id).loss_pending = true;
}

}  // namespace perfetto
