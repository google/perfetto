/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/protozero/proto_ring_buffer.h"

#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

namespace {
constexpr size_t kGrowBytes = 128 * 1024;

inline ProtoRingBuffer::Message FramingError() {
  ProtoRingBuffer::Message msg{};
  msg.fatal_framing_error = true;
  return msg;
}

// Tries to decode a length-delimited proto field from |start|.
// Returns a valid boundary if the preamble is valid and the length is within
// |end|, or an invalid message otherwise.
ProtoRingBuffer::Message TryReadProtoMessage(const uint8_t* start,
                                             const uint8_t* end) {
  namespace proto_utils = protozero::proto_utils;
  uint64_t field_tag = 0;
  auto* start_of_len = proto_utils::ParseVarInt(start, end, &field_tag);
  if (start_of_len == start)
    return ProtoRingBuffer::Message{};  // Not enough data.

  const uint32_t tag = field_tag & 0x07;
  if (tag !=
      static_cast<uint32_t>(proto_utils::ProtoWireType::kLengthDelimited)) {
    PERFETTO_ELOG("RPC framing error, unexpected msg tag 0x%xu", tag);
    return FramingError();
  }

  uint64_t msg_len = 0;
  auto* start_of_msg = proto_utils::ParseVarInt(start_of_len, end, &msg_len);
  if (start_of_msg == start_of_len)
    return ProtoRingBuffer::Message{};  // Not enough data.

  if (msg_len > ProtoRingBuffer::kMaxMsgSize) {
    PERFETTO_ELOG("RPC framing error, message too large (%" PRIu64 " > %zu)",
                  msg_len, ProtoRingBuffer::kMaxMsgSize);
    return FramingError();
  }

  if (start_of_msg + msg_len > end)
    return ProtoRingBuffer::Message{};  // Not enough data.

  ProtoRingBuffer::Message msg{};
  msg.start = start_of_msg;
  msg.len = static_cast<uint32_t>(msg_len);
  msg.field_id = static_cast<uint32_t>(field_tag >> 3);
  return msg;
}

}  // namespace

ProtoRingBuffer::ProtoRingBuffer()
    : buf_(perfetto::base::PagedMemory::Allocate(kGrowBytes)) {}
ProtoRingBuffer::~ProtoRingBuffer() = default;

ProtoRingBuffer::WriteHandle::~WriteHandle() {
  PERFETTO_CHECK(buffer_ == nullptr);
}

ProtoRingBuffer::WriteHandle::WriteHandle(WriteHandle&& other) noexcept
    : buffer_(other.buffer_), data_(other.data_), size_(other.size_) {
  other.buffer_ = nullptr;
}

ProtoRingBuffer::WriteHandle& ProtoRingBuffer::WriteHandle::operator=(
    WriteHandle&& other) noexcept {
  this->~WriteHandle();  // CHECKs that any reservation held was consumed.
  new (this) WriteHandle(std::move(other));
  return *this;
}

ProtoRingBuffer::WriteHandle ProtoRingBuffer::BeginWrite(size_t data_len) {
  PERFETTO_CHECK(data_len <= kMaxMsgSize);
  // A second reservation could recompact or grow the buffer under the first.
  PERFETTO_CHECK(!write_in_flight_);
  PERFETTO_DCHECK(wr_ <= buf_.size());
  PERFETTO_DCHECK(wr_ >= rd_);

  // Nothing can be tokenized any more, so recycle the whole buffer for the
  // bytes EndWrite() is about to drop.
  if (PERFETTO_UNLIKELY(failed_))
    rd_ = wr_ = 0;

  // If the last call to ReadMessage() consumed all the data in the buffer and
  // there are no incomplete messages pending, restart from the beginning rather
  // than keep ringing. This is the most common case.
  if (PERFETTO_LIKELY(rd_ == wr_))
    rd_ = wr_ = 0;

  size_t avail = buf_.size() - wr_;
  if (data_len > avail) {
    // This whole section should be hit extremely rarely.

    // Try first just recompacting the buffer by moving everything to the left.
    // This can happen if we received "a message and a bit" on each write call
    // so we ended pup in a situation like:
    // buf_: [unused space] [msg1 incomplete]
    //                      ^rd_             ^wr_
    //
    // After recompaction:
    // buf_: [msg1 incomplete]
    //       ^rd_             ^wr_
    uint8_t* buf = static_cast<uint8_t*>(buf_.Get());
    memmove(&buf[0], &buf[rd_], wr_ - rd_);
    avail += rd_;
    wr_ -= rd_;
    rd_ = 0;
    if (data_len > avail) {
      // The compaction didn't free up enough space and we need to expand the
      // ring buffer. Yes, we could have detected this earlier and split the
      // code paths, rather than first compacting and then realizing it wasn't
      // sufficient. However, that would make the code harder to reason about,
      // creating code paths that are nearly never hit, hence making it more
      // likely to accumulate bugs in future. All this is very rare.
      size_t new_size = buf_.size();
      while (data_len > new_size - wr_)
        new_size += kGrowBytes;
      if (new_size > kMaxMsgSize * 2) {
        // These bytes can never amount to a message (e.g. a never-ending
        // varint). |buf_| exceeds kMaxMsgSize by now, so dropping them leaves
        // room for the write, which EndWrite() will then discard.
        failed_ = true;
        rd_ = wr_ = 0;
        write_in_flight_ = true;
        return WriteHandle(this, static_cast<uint8_t*>(buf_.Get()), data_len);
      }
      auto new_buf = perfetto::base::PagedMemory::Allocate(new_size);
      memcpy(new_buf.Get(), buf_.Get(), buf_.size());
      buf_ = std::move(new_buf);
      // No need to touch rd_ / wr_ cursors.
    }
  }

  write_in_flight_ = true;
  return WriteHandle(this, static_cast<uint8_t*>(buf_.Get()) + wr_, data_len);
}

void ProtoRingBuffer::WriteHandle::EndWrite(size_t size_written) {
  PERFETTO_CHECK(buffer_ != nullptr);
  PERFETTO_CHECK(size_written <= size_);
  ProtoRingBuffer* buffer = buffer_;
  buffer_ = nullptr;
  buffer->FinishWrite(size_written);
}

void ProtoRingBuffer::WriteHandle::AbortWrite() {
  PERFETTO_CHECK(buffer_ != nullptr);
  ProtoRingBuffer* buffer = buffer_;
  buffer_ = nullptr;
  buffer->FinishWrite(0);
}

void ProtoRingBuffer::FinishWrite(size_t size_written) {
  write_in_flight_ = false;
  if (PERFETTO_LIKELY(!failed_))
    wr_ += size_written;
}

ProtoRingBuffer::Message ProtoRingBuffer::ReadMessage() {
  if (failed_)
    return FramingError();

  uint8_t* buf = static_cast<uint8_t*>(buf_.Get());

  PERFETTO_DCHECK(rd_ <= wr_);
  if (rd_ >= wr_)
    return Message{};  // Completely empty.

  auto msg = TryReadProtoMessage(&buf[rd_], &buf[wr_]);
  if (!msg.valid()) {
    failed_ = failed_ || msg.fatal_framing_error;
    return msg;  // Return |msg| because it could be a framing error.
  }

  const uint8_t* msg_end = msg.start + msg.len;
  PERFETTO_CHECK(msg_end > &buf[rd_] && msg_end <= &buf[wr_]);
  auto msg_outer_len = static_cast<size_t>(msg_end - &buf[rd_]);
  rd_ += msg_outer_len;
  return msg;
}

}  // namespace protozero
