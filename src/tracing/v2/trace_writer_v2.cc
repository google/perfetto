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

#include <stdint.h>

#include <functional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/root_message.h"
#include "src/tracing/v2/shared_ring_buffer.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

// Avoid starting a fragment without room for protozero's largest scalar field.
constexpr uint32_t kMinFragmentPayloadSize =
    protozero::proto_utils::kMaxSimpleFieldEncodedSize;

}  // namespace

TraceWriterV2::Delegate::~Delegate() = default;

TraceWriterV2::TraceWriterV2(const InitArgs& args)
    : delegate_(args.delegate),
      ring_buffer_writer_(args.ring_buffer,
                          args.writer_id,
                          args.target_buffer,
                          args.buffer_exhausted_policy,
                          delegate_.get()),
      stream_writer_(this),
      current_packet_(
          new protozero::RootMessage<protos::pbzero::TracePacket>()) {
  PERFETTO_DCHECK(args.ring_buffer);
  PERFETTO_DCHECK(args.delegate);

  // Protozero never receives more than one chunk at a time. Reserving the same
  // amount here keeps the drop path allocation-free.
  drop_buffer_.resize(args.ring_buffer->chunk_size());
  stream_writer_.Reset(
      {drop_buffer_.data(), drop_buffer_.data() + drop_buffer_.size()});
}

TraceWriterV2::~TraceWriterV2() {
  FinishTracePacket();
  // Publish before OnWriterDestroyed() samples the WriterID's retirement
  // position.
  ring_buffer_writer_.FinishCurrentChunk();
  stream_writer_.Reset({nullptr, nullptr});
  delegate_->OnWriterDestroyed(writer_id());
}

TraceWriter::TracePacketHandle TraceWriterV2::NewTracePacket() {
  // Starting another packet while its handle is alive would let the old
  // handle finalize the new packet when it eventually goes out of scope.
  PERFETTO_CHECK(!packet_open_);

  const SharedRingBufferWriter::FragmentRange range =
      ring_buffer_writer_.BeginFragment(kMinFragmentPayloadSize,
                                        /*continues_from_prev=*/false);
  if (range.result == SharedRingBufferWriter::BeginFragmentResult::kSuccess) {
    fragment_begin_ = range.begin;
    stream_writer_.Reset({range.begin, range.end});
    in_drop_mode_ = false;
  } else {
    stream_writer_.Reset(EnterDropMode());
  }

  // Every nested message inherits the root's encoding.
  current_packet_->Reset(&stream_writer_,
                         protozero::NestedMessageEncoding::kProtoGroup);
  packet_open_ = true;
  if (PERFETTO_UNLIKELY(first_packet_on_sequence_)) {
    current_packet_->set_first_packet_on_sequence(true);
    first_packet_on_sequence_ = false;
  }

  TracePacketHandle handle(current_packet_.get());
  handle.set_finalization_listener(this);
  return handle;
}

void TraceWriterV2::FinishTracePacket() {
  if (!packet_open_)
    return;

  current_packet_->Finalize();
  packet_open_ = false;
  ClosePacketFragment(/*continues_on_next=*/false);

  // Acquiring the fragment may have left holes before it. The reader has to
  // resolve those positions before it can reach this packet.
  delegate_->NotifyReader();
}

void TraceWriterV2::Flush(std::function<void()> callback) {
  // Flush must not invalidate memory still reachable through a packet handle.
  PERFETTO_CHECK(!packet_open_);

  // A completed fragment leaves its chunk cached so that the next packet can
  // append to it. Give up that cache before the delegate samples write_pos.
  ring_buffer_writer_.FinishCurrentChunk();
  stream_writer_.Reset({nullptr, nullptr});
  delegate_->Flush(writer_id(), std::move(callback));
}

void TraceWriterV2::OnMessageFinalized(protozero::Message*) {
  FinishTracePacket();
}

protozero::ContiguousMemoryRange TraceWriterV2::GetNewBuffer() {
  if (!packet_open_) {
    // protozero asked for space outside a packet. That only happens if a
    // caller writes through a stale handle, which is a data-source bug.
    return EnterDropMode();
  }

  // Once any part of a packet is lost, its remaining bytes are not useful.
  // Keep the packet in the drop buffer and try the ring again when the next
  // packet starts.
  if (in_drop_mode_)
    return EnterDropMode();

  // The packet did not fit. Publish the prefix as a continuing fragment and
  // carry on in another chunk.
  ClosePacketFragment(/*continues_on_next=*/true);

  // Notify before reserving the continuation. A packet can fill the entire
  // ring without finishing; a stalling writer would otherwise wait for space
  // before the reader had been asked to release any.
  delegate_->NotifyReader();

  if (in_drop_mode_)
    return EnterDropMode();

  const SharedRingBufferWriter::FragmentRange range =
      ring_buffer_writer_.BeginFragment(kMinFragmentPayloadSize,
                                        /*continues_from_prev=*/true);
  if (range.result != SharedRingBufferWriter::BeginFragmentResult::kSuccess)
    return EnterDropMode();

  fragment_begin_ = range.begin;
  return {range.begin, range.end};
}

uint8_t* TraceWriterV2::AnnotatePatch(uint8_t*) {
  PERFETTO_FATAL("TraceWriterV2 cannot patch previously written bytes");
}

void TraceWriterV2::ClosePacketFragment(bool continues_on_next) {
  if (!fragment_begin_) {
    // The packet went to the drop buffer. The next published chunk will carry
    // the data-loss flag.
    return;
  }

  const uint32_t used =
      static_cast<uint32_t>(stream_writer_.write_ptr() - fragment_begin_);
  fragment_begin_ = nullptr;

  // A failed relocation leaves this packet with a gap; finish it in the drop
  // buffer. SharedRingBufferWriter reports the loss on its next chunk.
  const SharedRingBufferWriter::EndFragmentResult result =
      ring_buffer_writer_.EndFragment(used, continues_on_next);
  if (result == SharedRingBufferWriter::EndFragmentResult::kRelocationDropped) {
    in_drop_mode_ = true;
    ++drop_count_;
  }
}

protozero::ContiguousMemoryRange TraceWriterV2::EnterDropMode() {
  fragment_begin_ = nullptr;

  if (!in_drop_mode_) {
    in_drop_mode_ = true;
    ++drop_count_;
    // Report the gap on the next chunk this writer manages to publish.
    ring_buffer_writer_.RecordDataLoss();
  }

  return {drop_buffer_.data(), drop_buffer_.data() + drop_buffer_.size()};
}

}  // namespace perfetto::tracing_v2
