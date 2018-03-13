/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/core/trace_writer_impl.h"

#include <string.h>

#include <algorithm>
#include <type_traits>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"

#include "perfetto/trace/trace_packet.pbzero.h"

// TODO(primiano): right now this class is accumulating a patchlist but not
// sending it to the service.

using protozero::proto_utils::kMessageLengthFieldSize;
using protozero::proto_utils::WriteRedundantVarInt;
using ChunkHeader = perfetto::SharedMemoryABI::ChunkHeader;

namespace perfetto {

namespace {
constexpr size_t kPacketHeaderSize = SharedMemoryABI::kPacketHeaderSize;
}  // namespace

TraceWriterImpl::TraceWriterImpl(SharedMemoryArbiterImpl* shmem_arbiter,
                                 WriterID id,
                                 BufferID target_buffer)
    : shmem_arbiter_(shmem_arbiter),
      id_(id),
      target_buffer_(target_buffer),
      protobuf_stream_writer_(this) {
  // TODO(primiano): we could handle the case of running out of TraceWriterID(s)
  // more gracefully and always return a no-op TracePacket in NewTracePacket().
  PERFETTO_CHECK(id_ != 0);

  cur_packet_.reset(new protos::pbzero::TracePacket());
  cur_packet_->Finalize();  // To avoid the DCHECK in NewTracePacket().
}

TraceWriterImpl::~TraceWriterImpl() {
  cur_packet_->Finalize();
  Flush();
  shmem_arbiter_->ReleaseWriterID(id_);
}

void TraceWriterImpl::Flush() {
  // Flush() cannot be called in the middle of a TracePacket.
  PERFETTO_CHECK(cur_packet_->is_finalized());
  if (cur_chunk_.is_valid()) {
    shmem_arbiter_->ReturnCompletedChunk(std::move(cur_chunk_), target_buffer_);
    // TODO(primiano): In next CL:
    // shmem_arbiter_->FlushPendingCommitDataRequests();
  }
  protobuf_stream_writer_.Reset({nullptr, nullptr});
}

TraceWriterImpl::TracePacketHandle TraceWriterImpl::NewTracePacket() {
  // If we hit this, the caller is calling NewTracePacket() without having
  // finalized the previous packet.
  PERFETTO_DCHECK(cur_packet_->is_finalized());

  fragmenting_packet_ = false;

  // TODO(fmayer): hack to get a new page every time and reduce fragmentation
  // (that requires stitching support in the service).
  protobuf_stream_writer_.Reset(GetNewBuffer());

  // Reserve space for the size of the message. Note: this call might re-enter
  // into this class invoking GetNewBuffer() if there isn't enough space or if
  // this is the very first call to NewTracePacket().
  static_assert(kPacketHeaderSize == kMessageLengthFieldSize,
                "The packet header must match the Message header size");
  cur_packet_->Reset(&protobuf_stream_writer_);
  uint8_t* header = protobuf_stream_writer_.ReserveBytes(kPacketHeaderSize);
  memset(header, 0, kPacketHeaderSize);
  cur_packet_->set_size_field(header);
  cur_chunk_.IncrementPacketCount();
  TracePacketHandle handle(cur_packet_.get());
  cur_fragment_start_ = protobuf_stream_writer_.write_ptr();
  fragmenting_packet_ = true;
  return handle;
}

// Called by the Message. We can get here in two cases:
// 1. In the middle of writing a Message,
// when |fragmenting_packet_| == true. In this case we want to update the
// chunk header with a partial packet and start a new partial packet in the
// new chunk.
// 2. While calling ReserveBytes() for the packet header in NewTracePacket().
// In this case |fragmenting_packet_| == false and we just want a new chunk
// without creating any fragments.
protozero::ContiguousMemoryRange TraceWriterImpl::GetNewBuffer() {
  if (fragmenting_packet_) {
    uint8_t* const wptr = protobuf_stream_writer_.write_ptr();
    PERFETTO_DCHECK(wptr >= cur_fragment_start_);
    uint32_t partial_size = static_cast<uint32_t>(wptr - cur_fragment_start_);
    PERFETTO_DCHECK(partial_size < cur_chunk_.size());

    // Backfill the packet header with the fragment size.
    cur_packet_->inc_size_already_written(partial_size);
    cur_chunk_.SetFlag(ChunkHeader::kLastPacketContinuesOnNextChunk);
    WriteRedundantVarInt(partial_size, cur_packet_->size_field());

    // Descend in the stack of non-finalized nested submessages (if any) and
    // detour their |size_field| into the |patch_list_|. At this point we have
    // to release the chunk and they cannot write anymore into that.
    // TODO(primiano): add tests to cover this logic.
    for (auto* nested_msg = cur_packet_->nested_message(); nested_msg;
         nested_msg = nested_msg->nested_message()) {
      uint8_t* const cur_hdr = nested_msg->size_field();
#if PERFETTO_DCHECK_IS_ON()
      // Ensure that the size field of the nested message either points to
      // somewhere in the current chunk or a size field of a patch in the
      // patch list.
      bool size_in_current_chunk =
          cur_hdr >= cur_chunk_.payload_begin() &&
          cur_hdr + kMessageLengthFieldSize <= cur_chunk_.end();
      if (!size_in_current_chunk) {
        auto patch_it = std::find_if(
            patch_list_.begin(), patch_list_.end(),
            [cur_hdr](const Patch& p) { return &p.size_field[0] == cur_hdr; });
        PERFETTO_DCHECK(patch_it != patch_list_.end());
      }
#endif
      auto offset = static_cast<uint16_t>(cur_hdr - cur_chunk_.payload_begin());
      Patch* patch = patch_list_.emplace_back(cur_chunk_id_, offset);
      nested_msg->set_size_field(&patch->size_field[0]);
      PERFETTO_DLOG("Created new patchlist entry for protobuf nested message");
    }
  }

  if (cur_chunk_.is_valid())
    shmem_arbiter_->ReturnCompletedChunk(std::move(cur_chunk_), target_buffer_);

  // Start a new chunk.

  ChunkHeader::Packets packets = {};
  if (fragmenting_packet_) {
    packets.count = 1;
    packets.flags = ChunkHeader::kFirstPacketContinuesFromPrevChunk;
  }

  // The memory order of the stores below doesn't really matter. This |header|
  // is just a local temporary object. The GetNewChunk() call below will copy it
  // into the shared buffer with the proper barriers.
  ChunkHeader header = {};
  header.writer_id.store(id_, std::memory_order_relaxed);
  header.chunk_id.store(cur_chunk_id_++, std::memory_order_relaxed);
  header.packets.store(packets, std::memory_order_relaxed);

  cur_chunk_ = shmem_arbiter_->GetNewChunk(header);
  uint8_t* payload_begin = cur_chunk_.payload_begin();
  if (fragmenting_packet_) {
    cur_packet_->set_size_field(payload_begin);
    memset(payload_begin, 0, kPacketHeaderSize);
    payload_begin += kPacketHeaderSize;
    cur_fragment_start_ = payload_begin;
  }

  return protozero::ContiguousMemoryRange{payload_begin, cur_chunk_.end()};
}

WriterID TraceWriterImpl::writer_id() const {
  return id_;
};

// Base class ctor/dtor definition.
TraceWriter::TraceWriter() = default;
TraceWriter::~TraceWriter() = default;

}  // namespace perfetto
