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

#include "src/tracing/service/service_ring_buffer_endpoint.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "src/tracing/service/trace_buffer_v2.h"

namespace perfetto::tracing_v2 {

namespace {

// A drain that stops early posts one retry after this delay. So a writer
// that keeps winning chunk state transitions cannot keep the service sequence
// busy.
constexpr uint32_t kDrainRetryDelayMs = 1;

}  // namespace

ServiceRingBufferEndpoint::Delegate::~Delegate() = default;

ServiceRingBufferEndpoint::ServiceRingBufferEndpoint(
    std::unique_ptr<SharedMemory> memory,
    uint32_t chunk_size_bytes,
    ProducerID producer_id,
    ClientIdentity client_identity,
    Delegate* delegate,
    base::TaskRunner* task_runner)
    : producer_id_(producer_id),
      client_identity_(client_identity),
      delegate_(delegate),
      memory_(std::move(memory)),
      ring_buffer_(static_cast<uint8_t*>(memory_->start()),
                   memory_->size(),
                   chunk_size_bytes),
      reader_(&ring_buffer_, this),
      weak_runner_(task_runner) {}

ServiceRingBufferEndpoint::~ServiceRingBufferEndpoint() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

void ServiceRingBufferEndpoint::Drain() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (reader_.has_protocol_error())
    return;

  // One pass can consume every chunk once. Positions that writers reserve
  // during the pass also count.
  const SharedRingBufferReader::Stats stats_before = reader_.GetStats();
  const auto result = reader_.Drain(ring_buffer_.num_chunks());
  const SharedRingBufferReader::Stats stats_after = reader_.GetStats();

  // The reader discards malformed chunks and chunks in an unknown format. It
  // reports them as loss for the writer. Count them here, once per pass.
  const uint64_t num_rejected =
      (stats_after.malformed_chunks - stats_before.malformed_chunks) +
      (stats_after.unsupported_format_chunks -
       stats_before.unsupported_format_chunks);
  if (num_rejected)
    delegate_->OnRingBufferChunksDiscarded(num_rejected);

  if (reader_.has_protocol_error()) {
    // The reader stopped for good. The producer keeps writing until the ring
    // buffer is full. Then its writers drop packets, or stall and time out.
    // Mark the ABI violation on every destination of this producer, so that
    // the trace shows it.
    delegate_->ForEachRingBufferDestination(
        [](TraceBufferV2& buffer) { buffer.RecordAbiViolation(); });
    return;
  }

  if (result.needs_another_drain() && !retry_scheduled_) {
    retry_scheduled_ = true;
    weak_runner_.PostDelayedTask(
        [this] {
          // Clear the flag first. This Drain() can stop early again, and it
          // then must post the next retry.
          retry_scheduled_ = false;
          Drain();
        },
        kDrainRetryDelayMs);
  }
}

void ServiceRingBufferEndpoint::OnChunkRead(
    const SharedRingBufferReader::ChunkContents& chunk) {
  if (!chunk.writer_id || chunk.writer_id > kMaxWriterID) {
    delegate_->OnRingBufferChunksDiscarded(1);
    return;
  }
  TraceBufferV2* buffer =
      delegate_->GetRingBufferDestination(chunk.target_buffer);
  if (!buffer) {
    delegate_->OnRingBufferChunksDiscarded(1);
    RecordWriterLoss(chunk.writer_id);
    return;
  }
  // The reader reports a chunk with kFlagDataLoss through OnDataLoss(). Only
  // the continuation flags can reach this point.
  PERFETTO_DCHECK(!(chunk.payload_flags & kFlagDataLoss));
  const TraceBufferV2::PacketSequenceProperties sequence{
      producer_id_, client_identity_, chunk.writer_id};
  // On rejection, the trace buffer counts the chunk in its own statistics and
  // records the loss on the sequence.
  //
  // TODO(sashwinbalaji): the payload is copied twice: from shared memory into
  // the reader's scratch, then into a TBv2 chunk. The reader could pass the
  // decoded sizes and one payload range, so that TBv2 copies once. Measure
  // the drain cost in a profile first.
  buffer->AppendProtoGroupFragments(
      sequence, chunk.fragments, chunk.num_fragments,
      chunk.payload_flags & kFlagContinuesFromPrevChunk,
      chunk.payload_flags & kFlagContinuesOnNextChunk);
}

void ServiceRingBufferEndpoint::OnDataLoss(WriterID writer_id) {
  // The producer flagged this loss, or the reader discarded the chunk. Drain()
  // counts the discarded chunks. The flagged loss is not a service discard:
  // the sequence shows it as previous_packet_dropped.
  RecordWriterLoss(writer_id);
}

void ServiceRingBufferEndpoint::RecordWriterLoss(WriterID writer_id) {
  if (!writer_id || writer_id > kMaxWriterID)
    return;
  // The destination of a lost chunk is unknown. RecordProtoGroupLoss() changes
  // only a buffer that holds this writer's sequence.
  // - A writer has one destination, so at most one buffer matches.
  // - Before the writer's first stored chunk, no buffer matches.
  delegate_->ForEachRingBufferDestination(
      [this, writer_id](TraceBufferV2& buffer) {
        buffer.RecordProtoGroupLoss(producer_id_, writer_id);
      });
}

}  // namespace perfetto::tracing_v2
