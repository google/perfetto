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

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <cstdint>
#include <functional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/v2/producer_ring_buffer_endpoint.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

// A new fragment must fit Protozero's largest scalar field.
constexpr uint32_t kMinFragmentPayloadSize =
    static_cast<uint32_t>(protozero::proto_utils::kMaxSimpleFieldEncodedSize);

}  // namespace

TraceWriterV2Impl::TraceWriterV2Impl(
    ProducerRingBufferEndpoint* ring_buffer_endpoint,
    SharedRingBuffer* ring_buffer,
    WriterID id,
    BufferID target_buffer,
    BufferExhaustedPolicy policy)
    : ring_buffer_endpoint_(ring_buffer_endpoint),
      ring_buffer_writer_(ring_buffer,
                          id,
                          target_buffer,
                          policy,
                          ring_buffer_endpoint),
      stream_writer_(this),
      cur_packet_(std::make_unique<
                  protozero::RootMessage<protos::pbzero::TracePacket>>()),
      process_id_(base::GetProcessId()) {}

TraceWriterV2Impl::~TraceWriterV2Impl() {
  FinishTracePacket();
  // Publish the last chunk before the ring buffer endpoint releases this
  // WriterID.
  // - Ring buffer reservations keep the order if another ring buffer writer
  //   reuses the ID.
  // - Reuse by an SMB writer is not ordered yet. See the TODO in
  //   ProducerRingBufferEndpoint::OnWriterDestroyed().
  ring_buffer_writer_.FinishCurrentChunk();
  stream_writer_.Reset({nullptr, nullptr});
  ring_buffer_endpoint_->OnWriterDestroyed(writer_id());
}

TraceWriter::TracePacketHandle TraceWriterV2Impl::NewTracePacket() {
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  EnsurePacketClosed();

  const auto range =
      ring_buffer_writer_.BeginFragment(kMinFragmentPayloadSize,
                                        /*continues_from_prev=*/false);
  if (range.result == SharedRingBufferWriter::BeginFragmentResult::kSuccess) {
    fragment_begin_ = range.begin;
    stream_writer_.Reset({range.begin, range.end});
    in_drop_mode_ = false;
  } else {
    stream_writer_.Reset(EnterDropMode());
  }

  cur_packet_->ResetToProtoGroup(&stream_writer_);
  packet_open_ = true;
  if (PERFETTO_UNLIKELY(first_packet_on_sequence_)) {
    cur_packet_->set_first_packet_on_sequence(true);
    first_packet_on_sequence_ = false;
  }

  TracePacketHandle handle(cur_packet_.get());
  handle.set_finalization_listener(this);
  return handle;
}

void TraceWriterV2Impl::FinishTracePacket() {
  PERFETTO_DCHECK(process_id_ == base::GetProcessId());
  if (!packet_open_)
    return;

  // Finalize() can append closing bytes that cross a chunk boundary. Keep
  // packet_open_ true until it returns so GetNewBuffer() can obtain space for
  // those bytes. Then publish the final fragment without a continuation flag.
  cur_packet_->Finalize();
  packet_open_ = false;
  ClosePacketFragment(/*continues_on_next=*/false);
}

void TraceWriterV2Impl::Flush(std::function<void()> callback) {
  EnsurePacketClosed();

  // A completed fragment leaves its chunk cached for later packets. Release
  // that chunk before ring_buffer_endpoint_->Flush() so later packets use new
  // reservations.
  ring_buffer_writer_.FinishCurrentChunk();
  stream_writer_.Reset({nullptr, nullptr});
  ring_buffer_endpoint_->Flush(std::move(callback));
}

void TraceWriterV2Impl::OnMessageFinalized(protozero::Message*) {
  FinishTracePacket();
}

protozero::ContiguousMemoryRange TraceWriterV2Impl::GetNewBuffer() {
  if (!packet_open_) {
    // A stale handle can request space without an open packet. This caller
    // error violates the data-source API contract.
    PERFETTO_DFATAL("TraceWriterV2Impl: write outside an open packet");
    return EnterDropMode();
  }

  // If any part of a packet is lost, its remaining bytes are not useful.
  // Send the rest to the drop buffer. Try the ring buffer for the next packet.
  if (in_drop_mode_)
    return EnterDropMode();

  // The current range cannot satisfy the stream's request. Publish this
  // fragment with kFlagContinuesOnNextChunk and continue in another chunk.
  ClosePacketFragment(/*continues_on_next=*/true);

  // ClosePacketFragment() can fail to relocate after the reader requests a
  // rewrite. Discard the remainder instead of reserving a continuation.
  if (in_drop_mode_)
    return EnterDropMode();

  const auto range =
      ring_buffer_writer_.BeginFragment(kMinFragmentPayloadSize,
                                        /*continues_from_prev=*/true);
  if (range.result != SharedRingBufferWriter::BeginFragmentResult::kSuccess)
    return EnterDropMode();

  fragment_begin_ = range.begin;
  return {range.begin, range.end};
}

uint8_t* TraceWriterV2Impl::AnnotatePatch(uint8_t*) {
  PERFETTO_FATAL("TraceWriterV2Impl cannot patch previously written bytes");
}

void TraceWriterV2Impl::EnsurePacketClosed() {
  if (packet_open_ && cur_packet_->is_finalized())
    FinishTracePacket();
  PERFETTO_CHECK(!packet_open_);
}

void TraceWriterV2Impl::ClosePacketFragment(bool continues_on_next) {
  if (!fragment_begin_) {
    // The packet went to the drop buffer. The next publication carries
    // kFlagDataLoss, even if it reuses a cached chunk.
    return;
  }

  const uint32_t used =
      static_cast<uint32_t>(stream_writer_.write_ptr() - fragment_begin_);
  fragment_begin_ = nullptr;

  // EndFragment() can need space even though this fragment already has a chunk:
  // 1. The reader copies the published fragments while this fragment is open.
  // 2. It sets RewriteRequested and advances past the reservation.
  // 3. EndFragment() cannot publish here. It saves this fragment, acknowledges
  //    the request, and tries to publish the fragment in another chunk.
  const auto result = ring_buffer_writer_.EndFragment(used, continues_on_next);
  // If relocation fails, discard the packet's remaining bytes.
  // SharedRingBufferWriter reports the loss on its next publication.
  if (result == SharedRingBufferWriter::EndFragmentResult::kRelocationDropped) {
    in_drop_mode_ = true;
    ++drop_count_;
  }
}

protozero::ContiguousMemoryRange TraceWriterV2Impl::EnterDropMode() {
  fragment_begin_ = nullptr;

  // A relocation failure can set in_drop_mode_ before this buffer's first use.
  // Check the buffer itself so that path also allocates storage.
  if (drop_buffer_.empty())
    drop_buffer_.resize(ring_buffer_writer_.max_fragment_size());

  if (!in_drop_mode_) {
    in_drop_mode_ = true;
    ++drop_count_;
    ring_buffer_writer_.RecordDataLoss();
  }

  return {drop_buffer_.data(), drop_buffer_.data() + drop_buffer_.size()};
}

}  // namespace perfetto::tracing_v2
