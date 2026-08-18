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
#include "perfetto/protozero/root_message.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

// The smallest fragment worth handing to the encoder. Anything smaller and the
// packet would be split into pieces that cost more in continuation flags than
// they carry.
constexpr uint32_t kMinFragmentSize = 16;

}  // namespace

TraceWriterV2::Delegate::~Delegate() = default;

TraceWriterV2::TraceWriterV2(const InitArgs& args)
    : ring_writer_(args.ring_buffer,
                   args.writer_id,
                   args.target_buffer,
                   args.buffer_exhausted_policy),
      delegate_(args.delegate),
      stream_writer_(this),
      current_packet_(
          new protozero::RootMessage<protos::pbzero::TracePacket>()) {
  PERFETTO_DCHECK(args.ring_buffer);
  PERFETTO_DCHECK(args.delegate);
  // One chunk's worth is the most the encoder can ever be handed in one go, so
  // the drop path never has to grow or reallocate.
  drop_buffer_.resize(args.ring_buffer->chunk_size());
}

TraceWriterV2::~TraceWriterV2() {
  FinishTracePacket();
  // Hand the ring back whatever is still held. Nothing is posted and no
  // callback runs: a destroyed writer has no control-plane side effect in
  // Step 1.
  ring_writer_.Release();
}

TraceWriter::TracePacketHandle TraceWriterV2::NewTracePacket() {
  FinishTracePacket();

  const RingWriter::FragmentSpan span = ring_writer_.OpenFragment(
      kMinFragmentSize, /*continues_from_prev=*/false);
  if (span.outcome == RingWriter::Outcome::kOk) {
    fragment_begin_ = span.begin;
    stream_writer_.Reset({span.begin, span.end});
    dropping_ = false;
  } else {
    stream_writer_.Reset(EnterDropMode());
  }

  if (span.outcome == RingWriter::Outcome::kNoChunkAvailable) {
    // That outcome means the attempt reserved positions it could not claim.
    // They are holes only the reader can resolve, and until it does they count
    // against capacity - so on a small enough ring this writer has just made
    // itself full. Telling the reader now, rather than when this packet is
    // finished, is what keeps the next attempt from starving: a stalling writer
    // returns instead of waiting for exactly this reason, and something has to
    // make the reader run.
    delegate_->OnPacketsCommitted();
  }

  // The encoding is chosen here, once, and is immutable for this packet: every
  // nested message inherits it. Nothing may change it after this point.
  current_packet_->Reset(
      &stream_writer_,
      protozero::NestedMessageEncoding::kStartTagAndTerminator);
  packet_open_ = true;
  return TracePacketHandle(current_packet_.get());
}

void TraceWriterV2::FinishTracePacket() {
  if (!packet_open_)
    return;
  packet_open_ = false;

  current_packet_->Finalize();
  ClosePacketFragment(/*continues_on_next=*/false);

  // Every call here may have moved write_pos, including the ones that only
  // burned a reservation, and a burned reservation still has to be resolved
  // before the reader can pass it. Coalescing this into one drain pass is the
  // delegate's job.
  delegate_->OnPacketsCommitted();
}

void TraceWriterV2::ClosePacketFragment(bool continues_on_next) {
  if (!fragment_begin_) {
    // The packet went to the drop buffer. Nothing was reserved for it, so
    // there is nothing to close; the gap is reported on the next chunk this
    // writer publishes.
    return;
  }

  const uint32_t used =
      static_cast<uint32_t>(stream_writer_.write_ptr() - fragment_begin_);
  written_ += used;
  fragment_begin_ = nullptr;

  const RingWriter::Outcome outcome =
      ring_writer_.CloseFragment(used, continues_on_next);
  if (outcome == RingWriter::Outcome::kRelocationDropped) {
    // The reader scraped the chunk and there was no replacement capacity. The
    // ring writer has already recorded the gap for the next publication.
    ++drop_count_;
  }
}

protozero::ContiguousMemoryRange TraceWriterV2::GetNewBuffer() {
  if (!packet_open_) {
    // protozero asked for space outside a packet. That only happens if a
    // caller writes through a stale handle, which is a data-source bug.
    return EnterDropMode();
  }

  // The packet did not fit in its chunk. Close what it filled, say that it
  // continues, and carry on in a fresh chunk.
  //
  // If that close had to drop what it was holding, the next chunk still claims
  // to continue something. That is deliberate: the reader recognises a
  // continuation with no beginning, drops the tail and reports the gap, which
  // is better than letting half a packet through as if it were whole.
  ClosePacketFragment(/*continues_on_next=*/true);

  // Before asking for the continuation, not after. A packet larger than the
  // whole ring fills every chunk in it without ever finishing, so this is the
  // only point at which the reader can be told there is something to drain. A
  // stalling writer that skipped it would wait for capacity that nothing has
  // been asked to produce; a dropping one would throw the rest of the packet
  // away with a reader that never ran. The delegate is what coalesces this into
  // one drain pass, which is why it is not a post from here.
  delegate_->OnPacketsCommitted();

  const RingWriter::FragmentSpan span =
      ring_writer_.OpenFragment(kMinFragmentSize, /*continues_from_prev=*/true);
  if (span.outcome != RingWriter::Outcome::kOk)
    return EnterDropMode();

  fragment_begin_ = span.begin;
  return {span.begin, span.end};
}

protozero::ContiguousMemoryRange TraceWriterV2::EnterDropMode() {
  fragment_begin_ = nullptr;
  if (!dropping_) {
    dropping_ = true;
    ++drop_count_;
    // The next chunk this writer manages to publish reports the gap, so that a
    // consumer can tell a truncated sequence from a complete one.
    ring_writer_.RecordDataLoss();
  }
  return {drop_buffer_.data(), drop_buffer_.data() + drop_buffer_.size()};
}

uint8_t* TraceWriterV2::AnnotatePatch(uint8_t*) {
  // The private framing never writes a length ahead of the payload it
  // describes, so nothing is ever patched. Returning null tells protozero not
  // to write to the address.
  PERFETTO_DCHECK(false);
  return nullptr;
}

void TraceWriterV2::Flush(std::function<void()> callback) {
  FinishTracePacket();
  // Publishing whatever is held is what makes the data reachable by the relay
  // at all; without it the last chunk stays Acquired until the writer's next
  // packet.
  ring_writer_.Release();
  delegate_->OnPacketsCommitted();

  // Run inline. See the header: this is not an acknowledgement that anything
  // reached the service, because Step 1 has no path for one to come back.
  if (callback)
    callback();
}

WriterID TraceWriterV2::writer_id() const {
  return ring_writer_.writer_id();
}

uint64_t TraceWriterV2::written() const {
  return written_;
}

uint64_t TraceWriterV2::drop_count() const {
  return drop_count_;
}

}  // namespace perfetto::tracing_v2
