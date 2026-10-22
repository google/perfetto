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

#include <algorithm>
#include <atomic>
#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

namespace {
constexpr size_t kGrowBytes = 128 * 1024;

// The boundaries of a message inside the buffer, before it is turned into a
// Message holding a reference to that buffer.
struct Token {
  const uint8_t* start = nullptr;
  uint32_t len = 0;
  uint32_t field_id = 0;
  bool fatal_framing_error = false;
  bool valid() const { return !!start; }
};

inline Token FramingError() {
  Token msg{};
  msg.fatal_framing_error = true;
  return msg;
}

// Tries to decode a length-delimited proto field from |start|.
// Returns a valid boundary if the preamble is valid and the length is within
// |end|, or an invalid message otherwise.
Token TryReadProtoMessage(const uint8_t* start, const uint8_t* end) {
  namespace proto_utils = protozero::proto_utils;
  uint64_t field_tag = 0;
  auto* start_of_len = proto_utils::ParseVarInt(start, end, &field_tag);
  if (start_of_len == start)
    return Token{};  // Not enough data.

  const uint32_t tag = field_tag & 0x07;
  if (tag !=
      static_cast<uint32_t>(proto_utils::ProtoWireType::kLengthDelimited)) {
    PERFETTO_ELOG("RPC framing error, unexpected msg tag 0x%xu", tag);
    return FramingError();
  }

  uint64_t msg_len = 0;
  auto* start_of_msg = proto_utils::ParseVarInt(start_of_len, end, &msg_len);
  if (start_of_msg == start_of_len)
    return Token{};  // Not enough data.

  if (msg_len > ProtoRingBuffer::kMaxMsgSize) {
    PERFETTO_ELOG("RPC framing error, message too large (%" PRIu64 " > %zu)",
                  msg_len, ProtoRingBuffer::kMaxMsgSize);
    return FramingError();
  }

  if (start_of_msg + msg_len > end)
    return Token{};  // Not enough data.

  Token msg{};
  msg.start = start_of_msg;
  msg.len = static_cast<uint32_t>(msg_len);
  msg.field_id = static_cast<uint32_t>(field_tag >> 3);
  return msg;
}

}  // namespace

// The memory messages are tokenized out of. Refcounted, because the messages
// handed out are slices of it and may outlive the ring buffer.
class ProtoRingBuffer::Buffer {
 public:
  static BufferPtr Create(size_t capacity) {
    return BufferPtr(new Buffer(capacity));
  }
  BufferPtr Share() {
    refs_.fetch_add(1, std::memory_order_relaxed);
    return BufferPtr(this);
  }

  // True if no Message points into this buffer, i.e. its bytes can be recycled
  // or moved around.
  bool IsUniquelyOwned() const {
    return refs_.load(std::memory_order_acquire) == 1;
  }

  uint8_t* data() { return static_cast<uint8_t*>(mem_.Get()); }
  size_t size() const { return mem_.size(); }

 private:
  friend struct BufferDeleter;

  explicit Buffer(size_t capacity)
      : mem_(perfetto::base::PagedMemory::Allocate(capacity)) {}

  void Release() {
    if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }

  std::atomic<uint32_t> refs_{1};
  perfetto::base::PagedMemory mem_;
};

void ProtoRingBuffer::BufferDeleter::operator()(Buffer* buffer) const {
  buffer->Release();
}

ProtoRingBuffer::Message::~Message() = default;

ProtoRingBuffer::Message::Message(Message&& other) noexcept {
  *this = std::move(other);
}

ProtoRingBuffer::Message& ProtoRingBuffer::Message::operator=(
    Message&& other) noexcept {
  if (this == &other)
    return *this;
  buf_ = std::move(other.buf_);
  start_ = std::exchange(other.start_, nullptr);
  len_ = std::exchange(other.len_, 0);
  field_id_ = std::exchange(other.field_id_, 0);
  fatal_framing_error_ = std::exchange(other.fatal_framing_error_, false);
  return *this;
}

ProtoRingBuffer::ProtoRingBuffer() : buf_(Buffer::Create(kGrowBytes)) {}
ProtoRingBuffer::~ProtoRingBuffer() = default;

size_t ProtoRingBuffer::cached_capacity_for_testing() const {
  return spare_ ? spare_->size() : 0;
}

ProtoRingBuffer::BufferPtr ProtoRingBuffer::AcquireBuffer(size_t capacity) {
  if (spare_ && spare_->IsUniquelyOwned() && spare_->size() >= capacity)
    return std::move(spare_);
  ++num_buffers_;
  return Buffer::Create(capacity);
}

void ProtoRingBuffer::RecycleBuffer(BufferPtr buffer) {
  // Keep only the most recent buffer, and only if it can satisfy future
  // replacements. In particular, growth must not cache an undersized buffer.
  spare_.reset();
  if (buffer->size() >= buf_->size())
    spare_ = std::move(buffer);
}

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
  PERFETTO_DCHECK(wr_ <= buf_->size());
  PERFETTO_DCHECK(wr_ >= rd_);

  const bool uniquely_owned = buf_->IsUniquelyOwned();
  // After a framing error, no unread bytes need preserving. Messages already
  // handed out still own their bytes, so rewinding requires exclusive
  // ownership.
  if (PERFETTO_UNLIKELY(failed_))
    rd_ = wr_;
  if (rd_ == wr_ && uniquely_owned)
    rd_ = wr_ = 0;

  if (PERFETTO_UNLIKELY(data_len > buf_->size() - wr_)) {
    size_t pending = wr_ - rd_;
    size_t required = pending + data_len;
    if (required > kMaxMsgSize * 2) {
      // These bytes can never amount to a message (e.g. a never-ending varint).
      // Reserve space for the write, which FinishWrite() will discard.
      failed_ = true;
      pending = 0;
      required = data_len;
    }

    if (uniquely_owned && required <= buf_->size()) {
      memmove(buf_->data(), buf_->data() + rd_, pending);
    } else {
      // Choose the final capacity before acquiring a buffer, so a write that
      // must preserve retained messages and grow copies unread bytes only once.
      const size_t capacity =
          std::max(buf_->size(),
                   ((required + kGrowBytes - 1) / kGrowBytes) * kGrowBytes);
      auto next = AcquireBuffer(capacity);
      memcpy(next->data(), buf_->data() + rd_, pending);
      auto previous = std::move(buf_);
      buf_ = std::move(next);
      RecycleBuffer(std::move(previous));
    }
    rd_ = 0;
    wr_ = pending;
  }

  write_in_flight_ = true;
  return WriteHandle(this, buf_->data() + wr_, data_len);
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
  // Token only carries boundaries; attaching |buf_| is what keeps the bytes
  // alive for as long as the caller holds the Message.
  auto make_message = [this](const Token& tok) {
    Message msg;
    msg.fatal_framing_error_ = tok.fatal_framing_error;
    if (tok.valid()) {
      msg.buf_ = buf_->Share();
      msg.start_ = tok.start;
      msg.len_ = tok.len;
      msg.field_id_ = tok.field_id;
    }
    return msg;
  };

  if (failed_)
    return make_message(FramingError());

  uint8_t* buf = buf_->data();

  PERFETTO_DCHECK(rd_ <= wr_);
  if (rd_ >= wr_)
    return Message{};  // Completely empty.

  auto msg = TryReadProtoMessage(&buf[rd_], &buf[wr_]);
  if (!msg.valid()) {
    failed_ = failed_ || msg.fatal_framing_error;
    return make_message(msg);  // Could still be a framing error.
  }

  const uint8_t* msg_end = msg.start + msg.len;
  PERFETTO_CHECK(msg_end > &buf[rd_] && msg_end <= &buf[wr_]);
  auto msg_outer_len = static_cast<size_t>(msg_end - &buf[rd_]);
  rd_ += msg_outer_len;
  return make_message(msg);
}

}  // namespace protozero
