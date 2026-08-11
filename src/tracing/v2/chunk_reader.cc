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

#include "src/tracing/v2/chunk_reader.h"

#include "perfetto/base/logging.h"
#include "src/tracing/v2/proto_ungrouper.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {

ChunkReader::ChunkReader(SharedRingBuffer* ring, size_t max_packet_size)
    : ring_(ring), max_packet_size_(max_packet_size) {
  PERFETTO_CHECK(ring_);
  PERFETTO_CHECK(max_packet_size_ > 0);
}

void ChunkReader::MarkLoss(WriterState* state, uint32_t reasons) {
  if (state->has_pending_fragment) {
    ++stats_.packets_lost;
    state->pending_fragment.clear();
    state->has_pending_fragment = false;
  }
  // DATA_LOSS_PRESENT accompanies every specific reason, by definition of the
  // field.
  state->pending_loss |=
      reasons | protos::pbzero::TracePacket::DATA_LOSS_PRESENT;
}

bool ChunkReader::AppendPending(WriterState* state,
                                const uint8_t* data,
                                size_t size) {
  if (state->pending_fragment.size() > max_packet_size_ ||
      size > max_packet_size_ - state->pending_fragment.size()) {
    MarkLoss(state, protos::pbzero::TracePacket::DATA_LOSS_REASSEMBLY_GAP);
    return false;
  }
  state->pending_fragment.insert(state->pending_fragment.end(), data,
                                 data + size);
  return true;
}

bool ChunkReader::EmitPacket(
    WriterID writer_id,
    BufferID target_buffer,
    WriterState* state,
    const uint8_t* data,
    size_t size,
    const std::function<void(const Packet&)>& callback) {
  // Hoist any previous_packet_dropped the producer set itself, so ours is
  // merged with it instead of appearing as a second, overriding occurrence.
  uint64_t producer_loss = 0;
  if (size > max_packet_size_ ||
      !UngroupProtoBytes(
          data, data + size, max_packet_size_, &canonical_packet_,
          protos::pbzero::TracePacket::kPreviousPacketDroppedFieldNumber,
          &producer_loss)) {
    ++stats_.packets_lost;
    state->pending_loss |=
        protos::pbzero::TracePacket::DATA_LOSS_PRESENT |
        protos::pbzero::TracePacket::DATA_LOSS_CHUNK_CORRUPTED;
    return false;
  }
  uint32_t loss_reasons =
      state->pending_loss | static_cast<uint32_t>(producer_loss);
  if (loss_reasons != 0)
    loss_reasons |= protos::pbzero::TracePacket::DATA_LOSS_PRESENT;

  Packet packet;
  packet.writer_id = writer_id;
  packet.target_buffer = target_buffer;
  packet.previous_packet_dropped = loss_reasons;
  packet.data = canonical_packet_.data();
  packet.size = canonical_packet_.size();
  callback(packet);
  state->pending_loss = 0;
  ++stats_.packets_read;
  return true;
}

uint32_t ChunkReader::ProcessChunk(
    const ChunkHeader& header,
    const std::function<void(const Packet&)>& callback) {
  WriterState& state = writer_states_[header.writer_id];
  if (header.flags & kFlagDataLoss) {
    // The writer set this because it could not get a chunk, i.e. the ring was
    // full, which is exactly what DATA_LOSS_SMB_FULL means.
    MarkLoss(&state, protos::pbzero::TracePacket::DATA_LOSS_SMB_FULL);
  }

  const bool continues_from_previous =
      (header.flags & kFlagContinuesFromPrevChunk) != 0;
  const bool continues_on_next =
      (header.flags & kFlagContinuesOnNextChunk) != 0;

  // Continuation flags describe only the boundary records: the first record
  // can finish a packet started in an earlier chunk, and the last can start a
  // packet that finishes later. Records between them are complete packets.
  // Keeping this rule explicit is important when both flags are present and a
  // chunk contains more than one record.

  // Validate all records before emitting any packet from the chunk. A record
  // is [size][fragment bytes] and the list ends at the committed payload size;
  // whatever the slot holds past that belongs to a previous lap.
  //
  // TryReadChunk() drops a chunk that claims more than it can hold, so the
  // bound below always stays inside |payload_|.
  PERFETTO_DCHECK(header.payload_size <= kChunkPayloadSize);
  const size_t payload_size = header.payload_size;
  uint32_t num_records = 0;
  size_t offset = 0;
  while (offset < payload_size) {
    const size_t fragment_size = payload_[offset];
    if (fragment_size > payload_size - offset - kFragmentHeaderSize) {
      ++stats_.malformed_chunks;
      MarkLoss(&state, protos::pbzero::TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
      return 0;
    }
    offset += kFragmentHeaderSize + fragment_size;
    ++num_records;
  }

  if (num_records == 0 && (continues_from_previous || continues_on_next)) {
    ++stats_.malformed_chunks;
    MarkLoss(&state, protos::pbzero::TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
    return 0;
  }

  if (!continues_from_previous && state.has_pending_fragment) {
    MarkLoss(&state,
             protos::pbzero::TracePacket::DATA_LOSS_REASSEMBLY_BROKEN_CHAIN);
  }

  uint32_t packets_read = 0;
  offset = 0;
  for (uint32_t record = 0; record < num_records; ++record) {
    const size_t fragment_size = payload_[offset];
    const uint8_t* const fragment = payload_ + offset + kFragmentHeaderSize;
    const bool continues_from = record == 0 && continues_from_previous;
    const bool continues_to = record + 1 == num_records && continues_on_next;

    if (continues_from) {
      if (!state.has_pending_fragment ||
          state.target_buffer != header.target_buffer) {
        ++stats_.orphan_fragments;
        MarkLoss(&state,
                 protos::pbzero::TracePacket::DATA_LOSS_ORPHAN_CONTINUATION);
      } else if (AppendPending(&state, fragment, fragment_size) &&
                 !continues_to) {
        if (EmitPacket(header.writer_id, header.target_buffer, &state,
                       state.pending_fragment.data(),
                       state.pending_fragment.size(), callback)) {
          ++packets_read;
        }
        state.pending_fragment.clear();
        state.has_pending_fragment = false;
      }
    } else if (continues_to) {
      state.pending_fragment.assign(fragment, fragment + fragment_size);
      state.target_buffer = header.target_buffer;
      state.has_pending_fragment = true;
      if (state.pending_fragment.size() > max_packet_size_)
        MarkLoss(&state, protos::pbzero::TracePacket::DATA_LOSS_REASSEMBLY_GAP);
    } else if (EmitPacket(header.writer_id, header.target_buffer, &state,
                          fragment, fragment_size, callback)) {
      ++packets_read;
    }

    offset += kFragmentHeaderSize + fragment_size;
  }
  return packets_read;
}

ChunkReader::DrainResult ChunkReader::Drain(
    const std::function<void(const Packet&)>& callback) {
  DrainResult result;
  for (; result.chunks_processed < ring_->num_chunks();
       ++result.chunks_processed) {
    ChunkHeader header;
    const SharedRingBuffer::ReadResult read_result =
        ring_->TryReadChunk(&header, payload_);
    if (read_result == SharedRingBuffer::ReadResult::kNoData)
      break;
    // A skipped position still advances the FIFO cursor and therefore counts
    // against this pass's one-lap fairness bound.
    if (read_result == SharedRingBuffer::ReadResult::kChunkRead)
      result.packets_read += ProcessChunk(header, callback);
  }
  // Once per pass, not once per chunk: a stalled writer only needs to be told
  // that space exists, and waking it per chunk would be a syscall per chunk.
  if (result.chunks_processed != 0)
    ring_->NotifyReaderProgress();
  result.has_more = ring_->has_pending_data();
  return result;
}

void ChunkReader::ForgetWriter(WriterID writer_id) {
  WriterState* const state = writer_states_.Find(writer_id);
  if (!state)
    return;
  if (state->has_pending_fragment)
    ++stats_.packets_lost;
  writer_states_.Erase(writer_id);
}

}  // namespace tracing_v2
}  // namespace perfetto
