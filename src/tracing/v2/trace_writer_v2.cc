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

#include "src/tracing/v2/trace_writer_v2.h"

#include <string.h>
#include <cinttypes>

#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {

TraceWriterV2::Delegate::~Delegate() = default;

TraceWriterV2::TraceWriterV2(const InitArgs& args)
    : ring_buffer_(args.ring_buffer),
      delegate_(args.delegate),
      writer_id_(args.writer_id),
      target_buffer_(args.target_buffer),
      buffer_exhausted_policy_(args.buffer_exhausted_policy),
      stall_then_drop_timeout_ms_(args.stall_then_drop_timeout_ms),
      stall_fatal_timeout_ms_(args.stall_fatal_timeout_ms),
      protobuf_stream_writer_(this),
      process_id_(base::GetProcessId()) {
  PERFETTO_CHECK(ring_buffer_);
  PERFETTO_CHECK(writer_id_ != 0);

  protobuf_stream_writer_.set_serialization_flags(
      protozero::ScatteredStreamWriter::kNestedMessagesAsGroups);
  current_packet_.reset(
      new protozero::RootMessage<protos::pbzero::TracePacket>());
  current_packet_->Finalize();
}

TraceWriterV2::~TraceWriterV2() {
  if (!current_packet_->is_finalized())
    current_packet_->Finalize();
  if (packet_open_)
    FinishTracePacket();
  if (delegate_)
    delegate_->OnWriterDestroyed(writer_id_, ring_buffer_->write_pos());
}

WriterID TraceWriterV2::writer_id() const {
  return writer_id_;
}

uint64_t TraceWriterV2::written() const {
  return protobuf_stream_writer_.written();
}

uint64_t TraceWriterV2::drop_count() const {
  return drop_count_;
}

bool TraceWriterV2::TryAcquireChunk(uint8_t flags) {
  ChunkHeader header;
  header.writer_id = writer_id_;
  header.target_buffer = target_buffer_;
  header.flags = flags;
  if (pending_data_loss_)
    header.flags = static_cast<uint8_t>(header.flags | kFlagDataLoss);

  int64_t stall_started_ms = 0;
  for (;;) {
    // Sample the reader's progress counter *before* the attempt below. If the
    // reader frees a chunk in between, the wait sees a changed generation and
    // returns immediately instead of sleeping through the wakeup.
    const uint32_t reader_generation = ring_buffer_->reader_generation();

    current_chunk_ = ring_buffer_->TryAcquireChunkForWriting(header);
    if (PERFETTO_LIKELY(current_chunk_.is_valid())) {
      pending_data_loss_ = false;
      drop_packets_ = false;
      return true;
    }
    if (!StallForChunk(reader_generation, stall_started_ms))
      return false;
    if (stall_started_ms == 0)
      stall_started_ms = base::GetWallTimeMs().count();
  }
}

bool TraceWriterV2::StallForChunk(uint32_t reader_generation,
                                  int64_t stall_started_ms) {
  if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kDrop)
    return false;

  if (stall_started_ms != 0) {
    const int64_t stalled_for_ms =
        base::GetWallTimeMs().count() - stall_started_ms;
    if (buffer_exhausted_policy_ == BufferExhaustedPolicy::kStallThenDrop &&
        stalled_for_ms > static_cast<int64_t>(stall_then_drop_timeout_ms_)) {
      return false;
    }
    if (stalled_for_ms > static_cast<int64_t>(stall_fatal_timeout_ms_)) {
      // Only the relay frees ring chunks, so getting here means it is wedged,
      // or that unsupported re-entrant code is writing on the consumer
      // sequence. The ordinary v1 downstream flush path does not do that: it
      // posts its callback work to the muxer.
      PERFETTO_FATAL(
          "Tracing v2 writer stalled for %" PRId64
          " ms waiting for a chunk; the relay is wedged or this writer is "
          "running on the relay sequence",
          stalled_for_ms);
    }
  }

  // Wall clock rather than a retry count, because how often we get here says
  // nothing about how long the writer has actually been stuck. The timeout
  // only bounds one wait; the loop above re-checks the real predicate.
  constexpr uint32_t kWaitSliceMs = 100;
  ring_buffer_->WaitForReaderProgress(reader_generation, kWaitSliceMs);
  return true;
}

protozero::ContiguousMemoryRange TraceWriterV2::WritableChunkRange() const {
  PERFETTO_DCHECK(current_chunk_.is_valid() &&
                  current_chunk_.is_being_written());
  return protozero::ContiguousMemoryRange{
      current_chunk_.payload_begin() + current_chunk_.payload_used(),
      current_chunk_.payload_end()};
}

protozero::ContiguousMemoryRange TraceWriterV2::EnterDropMode() {
  if (!drop_packets_) {
    drop_packets_ = true;
    pending_data_loss_ = true;
    ++drop_count_;
  }
  fragment_size_field_ = nullptr;
  fragment_begin_ = nullptr;
  if (delegate_)
    delegate_->OnPacketsCommitted();  // Also drains reservation holes.
  return protozero::ContiguousMemoryRange{
      garbage_buffer_.data(), garbage_buffer_.data() + garbage_buffer_.size()};
}

void TraceWriterV2::StartFragment() {
  PERFETTO_DCHECK(!drop_packets_ && current_chunk_.is_being_written());
  PERFETTO_DCHECK(protobuf_stream_writer_.bytes_available() >=
                  kFragmentHeaderSize);
  fragment_size_field_ = protobuf_stream_writer_.ReserveBytesUnsafe(1);
  *fragment_size_field_ = 0;
  fragment_begin_ = protobuf_stream_writer_.write_ptr();
}

void TraceWriterV2::FinalizeFragment() {
  if (!fragment_size_field_)
    return;
  PERFETTO_DCHECK(current_chunk_.is_being_written());
  const size_t fragment_size = static_cast<size_t>(
      protobuf_stream_writer_.write_ptr() - fragment_begin_);
  PERFETTO_CHECK(fragment_size <= kMaxFragmentSize);
  *fragment_size_field_ = static_cast<uint8_t>(fragment_size);

  // Publishing this length is what makes the record visible; nothing marks the
  // end of the payload besides it.
  const size_t payload_used = static_cast<size_t>(
      protobuf_stream_writer_.write_ptr() - current_chunk_.payload_begin());
  current_chunk_.set_payload_used(static_cast<uint32_t>(payload_used));
  fragment_size_field_ = nullptr;
  fragment_begin_ = nullptr;
}

void TraceWriterV2::PublishCurrentChunk(uint8_t added_flags) {
  PERFETTO_DCHECK(current_chunk_.is_being_written());
  const bool released =
      ring_buffer_->ReleaseChunkAsComplete(&current_chunk_, added_flags);
  if (!released) {
    pending_data_loss_ = true;
    if (!drop_packets_)
      ++drop_count_;
    drop_packets_ = true;
  }
  if (delegate_)
    delegate_->OnPacketsCommitted();
}

TraceWriterV2::TracePacketHandle TraceWriterV2::NewTracePacket() {
  PERFETTO_CHECK(current_packet_->is_finalized());
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  PERFETTO_DCHECK(!packet_open_);

  // A loss marker must start a new chunk because an already-published header
  // cannot be changed. Otherwise take back the last chunk when it has enough
  // room for a size byte and useful payload. An empty packet would fit in the
  // size byte alone, but taking a chunk back to add nothing is not worth two
  // atomics and a reader retry.
  constexpr uint32_t kMinRecordSpace = kFragmentHeaderSize + 1;
  if (current_chunk_.is_valid() && !pending_data_loss_ &&
      current_chunk_.payload_free() >= kMinRecordSpace) {
    ring_buffer_->TryReacquireChunkForWriting(&current_chunk_);
  } else {
    current_chunk_ = SharedRingBuffer::Chunk();
  }

  if (!current_chunk_.is_being_written() && !TryAcquireChunk(/*flags=*/0)) {
    protobuf_stream_writer_.Reset(EnterDropMode());
  } else {
    protobuf_stream_writer_.Reset(WritableChunkRange());
    StartFragment();
  }

  current_packet_->Reset(&protobuf_stream_writer_);
  current_packet_->set_nested_messages_as_groups();
  packet_open_ = true;

  TracePacketHandle handle(current_packet_.get());
  handle.set_finalization_listener(this);
  return handle;
}

protozero::ContiguousMemoryRange TraceWriterV2::GetNewBuffer() {
  PERFETTO_DCHECK(packet_open_);
  if (drop_packets_)
    return EnterDropMode();

  FinalizeFragment();
  PublishCurrentChunk(kFlagContinuesOnNextChunk);
  if (drop_packets_ || !TryAcquireChunk(kFlagContinuesFromPrevChunk)) {
    return EnterDropMode();
  }

  protozero::ContiguousMemoryRange range = WritableChunkRange();
  // ScatteredStreamWriter installs the returned range only after this method
  // returns, so reserve the fragment-size byte manually from its beginning.
  fragment_size_field_ = range.begin;
  *fragment_size_field_ = 0;
  fragment_begin_ = range.begin + 1;
  range.begin = fragment_begin_;
  return range;
}

uint8_t* TraceWriterV2::AnnotatePatch(uint8_t*) {
  // C and C++ writers both switch nested messages to group encoding before
  // they can cross a chunk boundary. Reaching this means an unsupported raw
  // stream user attempted length backfilling on the v2 path.
  PERFETTO_DFATAL("Tracing v2 does not support out-of-band proto patches");
  return nullptr;
}

void TraceWriterV2::FinishTracePacket() {
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  if (!packet_open_)
    return;

  FinalizeFragment();
  if (!drop_packets_)
    PublishCurrentChunk(/*added_flags=*/0);

  packet_open_ = false;
  current_packet_->Reset(&protobuf_stream_writer_);
  current_packet_->Finalize();
}

void TraceWriterV2::Flush(std::function<void()> callback) {
  PERFETTO_CHECK(current_packet_->is_finalized());
  PERFETTO_CHECK(!packet_open_);
  if (delegate_) {
    delegate_->OnWriterFlush(writer_id_, ring_buffer_->write_pos(),
                             std::move(callback));
  } else if (callback) {
    callback();
  }
}

void TraceWriterV2::OnMessageFinalized(protozero::Message*) {
  FinishTracePacket();
}

}  // namespace tracing_v2
}  // namespace perfetto
