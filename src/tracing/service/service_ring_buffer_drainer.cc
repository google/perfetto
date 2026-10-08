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

#include "src/tracing/service/service_ring_buffer_drainer.h"

#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/thread_annotations.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "src/tracing/service/clock.h"
#include "src/tracing/service/trace_buffer_v2.h"

#include "protos/perfetto/trace/perfetto/tracing_v2_ring_buffer_dump.pbzero.h"

namespace perfetto::tracing_v2 {

namespace {

// Delay retries so writers that repeatedly win chunk state transitions
// cannot keep the service sequence busy.
constexpr uint32_t kDrainRetryDelayMs = 1;

// Upper bounds for DumpSizeBound(), in serialized bytes.
// - A field with a number below 2048 has a tag of at most 2 bytes, and a
//   length of at most 5 varint bytes.
// - The fixed part covers the TracePacket service fields, the length of the
//   dump field, every scalar field of the dump and the rejection record.
//   Each scalar takes at most 2 + 10 bytes, and nested lengths take 4 bytes.
constexpr size_t kDumpFieldOverhead = 7;
constexpr size_t kDumpFixedBytes = 512;
// A packed varint chunk index takes at most 5 bytes.
constexpr size_t kMaxChunkIndexBytes = 5;

}  // namespace

ServiceRingBufferDrainer::Delegate::~Delegate() = default;

ServiceRingBufferDrainer::ServiceRingBufferDrainer(
    std::shared_ptr<SharedMemory> memory,
    uint32_t chunk_size_bytes,
    ProducerID producer_id,
    ClientIdentity client_identity,
    Delegate* delegate,
    tracing_service::Clock* clock,
    base::TaskRunner* task_runner)
    : producer_id_(producer_id),
      client_identity_(client_identity),
      delegate_(delegate),
      clock_(clock),
      memory_(std::move(memory)),
      ring_buffer_(static_cast<uint8_t*>(memory_->start()),
                   memory_->size(),
                   chunk_size_bytes),
      reader_(&ring_buffer_, this),
      weak_runner_(task_runner) {}

ServiceRingBufferDrainer::~ServiceRingBufferDrainer() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

void ServiceRingBufferDrainer::Drain() {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  if (PERFETTO_UNLIKELY(reader_.has_protocol_error()))
    return;

  // One clock read per pass. The records of this pass use it.
  last_drain_.time_ns = clock_->GetBootTimeNs().count();

  // Bound each pass to num_chunks() logical positions, including positions
  // reserved during the pass.
  const SharedRingBufferReader::Stats stats_before = reader_.GetStats();
  const base::TimeNanos cpu_start = base::GetThreadCPUTimeNs();
  const auto result = reader_.Drain(ring_buffer_.num_chunks());
  stats_.drain_cpu_time_ns +=
      static_cast<uint64_t>((base::GetThreadCPUTimeNs() - cpu_start).count());
  const SharedRingBufferReader::Stats stats_after = reader_.GetStats();

  ++stats_.drain_passes;
  last_drain_.positions_consumed = result.positions_consumed;
  last_drain_.result = result.last_result;
  if (result.positions_consumed != 0) {
    last_drain_.progress_time_ns = last_drain_.time_ns;
  } else if (result.last_result ==
             SharedRingBufferReader::ConsumeResult::kRetryImmediately) {
    ++stats_.drain_passes_without_progress;
  }

  // The reader discards malformed chunks and chunks in an unknown format. It
  // reports them as loss for the writer. Count them here, once per pass.
  const uint64_t num_rejected =
      (stats_after.malformed_chunks - stats_before.malformed_chunks) +
      (stats_after.unsupported_format_chunks -
       stats_before.unsupported_format_chunks);
  if (PERFETTO_UNLIKELY(num_rejected != 0))
    delegate_->OnRingBufferChunksDiscarded(num_rejected);

  if (PERFETTO_UNLIKELY(reader_.has_protocol_error())) {
    protocol_error_time_ns_ = last_drain_.time_ns;
    // Record the error in every authorized destination. Later drains return
    // above, so each destination counts this violation only once.
    delegate_->ForEachRingBufferDestination(
        [](TraceBufferV2& buffer) { buffer.RecordAbiViolation(); });
    delegate_->OnRingBufferProtocolError();
    return;
  }

  if (result.needs_another_drain() && !retry_scheduled_) {
    retry_scheduled_ = true;
    weak_runner_.PostDelayedTask(
        [this] {
          // Clear the flag so Drain() can schedule another retry if needed.
          retry_scheduled_ = false;
          Drain();
        },
        kDrainRetryDelayMs);
  }
}

void ServiceRingBufferDrainer::OnChunkRead(
    const SharedRingBufferReader::ChunkContents& chunk) {
  // Each exit adds 1 to exactly one admission counter. See Stats.
  if (PERFETTO_UNLIKELY(!chunk.writer_id || chunk.writer_id > kMaxWriterID)) {
    ++stats_.invalid_writer_chunks;
    delegate_->OnRingBufferChunksDiscarded(1);
    return;
  }

  TraceBufferV2* buffer =
      delegate_->GetRingBufferDestination(chunk.target_buffer);
  if (PERFETTO_UNLIKELY(!buffer)) {
    ++stats_.invalid_destination_chunks;
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
  using Result = TraceBufferV2::CopyChunkV2Result;
  switch (buffer->CopyChunkV2Untrusted(
      sequence, chunk.fragments, chunk.num_fragments,
      chunk.payload_flags & kFlagContinuesFromPrevChunk,
      chunk.payload_flags & kFlagContinuesOnNextChunk)) {
    case Result::kAdmitted:
      ++stats_.chunks_admitted;
      stats_.admitted_payload_bytes += chunk.payload_size;
      return;
    case Result::kBufferFull:
      ++stats_.trace_buffer_rejected_buffer_full;
      return;
    case Result::kSequenceFormatConflict:
      ++stats_.trace_buffer_rejected_format_conflict;
      return;
    case Result::kProtoVmConflict:
      ++stats_.trace_buffer_rejected_protovm;
      return;
    case Result::kInvalidChunk:
      ++stats_.trace_buffer_rejected_invalid;
      return;
  }
}

size_t ServiceRingBufferDrainer::DumpSizeBound(
    bool include_chunk_bytes,
    std::optional<uint32_t> only_chunk_pos) const {
  const size_t num_chunks = only_chunk_pos ? 1 : ring_buffer_.num_chunks();
  size_t bound =
      kDumpFixedBytes + kDumpFieldOverhead + num_chunks * sizeof(uint32_t);
  if (include_chunk_bytes) {
    // The bytes, and the chunk index list of a partial copy.
    bound += kDumpFieldOverhead + num_chunks * ring_buffer_.chunk_size();
    bound += kDumpFieldOverhead + num_chunks * kMaxChunkIndexBytes;
  }
  return bound;
}

bool ServiceRingBufferDrainer::WriteDump(
    protos::pbzero::TracingV2RingBufferDump* dump,
    bool include_chunk_bytes,
    std::optional<uint32_t> only_chunk_pos,
    size_t max_bytes) const {
  using Dump = protos::pbzero::TracingV2RingBufferDump;
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const uint32_t num_chunks = ring_buffer_.num_chunks();
  const uint32_t chunk_size = ring_buffer_.chunk_size();

  // The required part: everything but the chunk bytes.
  const size_t required =
      DumpSizeBound(/*include_chunk_bytes=*/false, only_chunk_pos);
  if (required > max_bytes)
    return false;

  dump->set_chunk_size_bytes(chunk_size);
  dump->set_num_chunks(num_chunks);
  dump->set_reader_read_pos(reader_.read_pos());
  const SharedRingBuffer::HeaderSnapshot header =
      ring_buffer_.LoadHeaderRelaxed();
  dump->set_read_pos(header.read_pos);
  dump->set_write_pos(header.write_pos);
  dump->set_num_writers_waiting(header.num_writers_waiting);

  // The chunks of the dump, in position order: all chunks from the reader
  // position, or the one at |only_chunk_pos|.
  if (only_chunk_pos)
    dump->set_chunk_pos(*only_chunk_pos);
  const uint32_t first_pos = only_chunk_pos.value_or(reader_.read_pos());
  const uint32_t count = only_chunk_pos ? 1 : num_chunks;
  auto index_at = [&](uint32_t i) {
    return ChunkIndex::FromPosition(first_pos + i, num_chunks);
  };

  // Load each state word once. Chunk index order, except for one chunk.
  // A partial copy of chunk bytes chooses chunks by these same words, in
  // position order from |first_pos|.
  std::vector<uint32_t> words_by_position(include_chunk_bytes ? count : 0);
  protozero::PackedFixedSizeInt<uint32_t> state_words;
  for (uint32_t i = 0; i < count; ++i) {
    const ChunkIndex chunk_idx =
        only_chunk_pos ? index_at(0) : ChunkIndex::FromIndex(i);
    const uint32_t word = ring_buffer_.LoadChunkStateWordAcquire(chunk_idx);
    state_words.Append(word);
    if (include_chunk_bytes) {
      // num_chunks is a power of two, so the mask computes the distance from
      // |first_pos| modulo num_chunks.
      words_by_position[(chunk_idx.value() - first_pos) & (num_chunks - 1)] =
          word;
    }
  }
  dump->set_chunk_state_words(state_words);

  if (!include_chunk_bytes)
    return true;

  // All chunks, if they fit. They are contiguous in memory.
  // Writers can change the bytes during each copy, and nothing here parses
  // them.
  const size_t all_bytes = size_t{num_chunks} * chunk_size;
  if (!only_chunk_pos &&
      required + kDumpFieldOverhead + all_bytes <= max_bytes) {
    const uint8_t* chunks = ring_buffer_.chunk_at(ChunkIndex::FromIndex(0));
    PERFETTO_ANNOTATE_BENIGN_RACE_SIZED(chunks, all_bytes,
                                        "ring buffer dump of writer memory")
    dump->set_chunk_bytes(Dump::CHUNK_BYTES_ALL);
    dump->set_chunks(chunks, all_bytes);
    return true;
  }

  // Otherwise as many chunks as fit, each with its index: the chunk at the
  // reader position first, then the chunks that are not Free.
  const size_t per_chunk = chunk_size + kMaxChunkIndexBytes;
  const size_t fixed = required + 2 * kDumpFieldOverhead;
  const size_t max_selected =
      max_bytes > fixed ? (max_bytes - fixed) / per_chunk : 0;
  std::vector<protozero::ContiguousMemoryRange> ranges;
  protozero::PackedVarInt indexes;
  for (uint32_t i = 0; i < count && ranges.size() < max_selected; ++i) {
    if (i != 0 && ChunkStateOf(words_by_position[i]) == ChunkState::kFree)
      continue;
    const ChunkIndex chunk_idx = index_at(i);
    // AppendScatteredBytes() only reads the ranges.
    uint8_t* chunk = const_cast<uint8_t*>(ring_buffer_.chunk_at(chunk_idx));
    PERFETTO_ANNOTATE_BENIGN_RACE_SIZED(chunk, chunk_size,
                                        "ring buffer dump of writer memory")
    ranges.push_back({chunk, chunk + chunk_size});
    indexes.Append(chunk_idx.value());
  }
  const auto num_selected = static_cast<uint32_t>(ranges.size());
  dump->set_chunk_bytes(num_selected ? Dump::CHUNK_BYTES_SELECTED
                                     : Dump::CHUNK_BYTES_OMITTED);
  dump->set_chunk_bytes_omitted(count - num_selected);
  if (num_selected) {
    dump->AppendScatteredBytes(Dump::kChunksFieldNumber, ranges.data(),
                               ranges.size());
    dump->set_chunk_byte_indexes(indexes);
  }
  return true;
}

void ServiceRingBufferDrainer::OnChunkRejected(
    const SharedRingBufferReader::ChunkRejection& rejection) {
  std::optional<RejectionRecord>& first =
      first_rejections_[static_cast<size_t>(rejection.reason)];
  if (!first)
    first = RejectionRecord{rejection, last_drain_.time_ns};
  delegate_->OnRingBufferChunkRejected(rejection);
}

void ServiceRingBufferDrainer::OnDataLoss(WriterID writer_id) {
  // Record the loss in |writer_id|'s sequence so it can be reported through
  // previous_packet_dropped.
  //
  // Drain() counts chunks discarded by the reader in chunks_discarded.
  // Loss reported by the producer does not increase that counter.
  RecordWriterLoss(writer_id);
}

void ServiceRingBufferDrainer::RecordWriterLoss(WriterID writer_id) {
  if (!writer_id || writer_id > kMaxWriterID)
    return;
  // The destination of a lost chunk is unknown. RecordChunkV2DataLoss() changes
  // only a buffer that holds this writer's sequence.
  // - A writer has one destination, so at most one buffer matches.
  // - Before the writer's first stored chunk, no buffer matches.
  delegate_->ForEachRingBufferDestination(
      [this, writer_id](TraceBufferV2& buffer) {
        buffer.RecordChunkV2DataLoss(producer_id_, writer_id);
      });
}

}  // namespace perfetto::tracing_v2
