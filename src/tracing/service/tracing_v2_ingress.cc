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

#include <algorithm>
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
  return DrainUntil(write_pos());
}

uint32_t TracingV2Ingress::write_pos() const {
  return ring_->LoadWritePosRelaxed();
}

bool TracingV2Ingress::DrainUntil(uint32_t end_pos) {
  if (write_pos() - reader_.read_pos() > ring_->num_chunks()) {
    reader_.Drain(1);  // Latch and diagnose the invalid reservation window.
    return false;
  }
  const uint32_t remaining = end_pos - reader_.read_pos();
  // A previous inline or timeout drain can already have passed this boundary.
  if (static_cast<int32_t>(remaining) < 0)
    return reader_.Drain(0).needs_another_drain();
  reader_.Drain(std::min(remaining, kMaxPositionsPerDrain));
  return !has_protocol_error() && (reader_.read_pos() != end_pos ||
                                   reader_.Drain(0).needs_another_drain());
}

bool TracingV2Ingress::DrainToCompletion() {
  const uint32_t end_pos = write_pos();
  // Bound contention as well as the number of reservations. Healthy full
  // rings need at most 128 passes under the service's allocation limit.
  for (uint32_t pass = 0; pass < 512; ++pass) {
    if (!DrainUntil(end_pos))
      return !has_protocol_error();
  }
  return false;
}

void TracingV2Ingress::RetireWriter(WriterID writer_id) {
  // Preserve the ordinal while old fragments remain in TBv2 or a clone.
  // The next incarnation starts after a gap, so fragments cannot join it.
  GetWriterState(writer_id).loss_pending = true;
}

bool TracingV2Ingress::has_protocol_error() const {
  return reader_.has_protocol_error();
}

size_t TracingV2Ingress::GetMemoryUsageBytes() const {
  // Count allocated slots, including unused capacity and hash-table tags.
  // Pair padding and the extra bytes cover alignment without scanning entries.
  return reader_.GetMemoryUsageBytes() +
         frag_scratch_.capacity() * sizeof(TraceBufferV2::RingChunkFragment) +
         writers_.capacity() * (sizeof(std::pair<WriterID, WriterState>) + 1) +
         32;
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
  const ChunkID chunk_id = ws.next_chunk_id + (ws.loss_pending ? 1u : 0u);
  const TraceBufferV2::AdmitResult result = target->CopyRingChunkFragmentsV2(
      producer_id_, client_identity_, chunk.writer_id, chunk_id,
      frag_scratch_.data(), frag_scratch_.size(), cont_from_prev, cont_on_next,
      ws.loss_pending);

  if (result == TraceBufferV2::AdmitResult::kStored) {
    // Stored: this chunk carries any pending loss now, and takes the next id.
    ws.next_chunk_id = chunk_id + 1;
    ws.loss_pending = false;
  } else {
    // Keep a single pending gap until admission succeeds. Repeated failures
    // must not wrap the chunk counter and hide loss.
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
