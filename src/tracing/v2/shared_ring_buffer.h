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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto {

class SharedRingBuffer_Writer;
class SharedRingBuffer_Reader;

// This class implements the V2 tracing protocol ring buffer ABI.
// See rfcs/0014-tracing-protocol-redesign.md for the design document.
//
// Memory layout:
// +------------------+
// | RB Header (16B)  |
// +------------------+
// | Chunk 0 (256B)   |  <- Header (4B) + Payload (252B)
// +------------------+
// | Chunk 1 (256B)   |
// +------------------+
// |       ...        |
// +------------------+
class SharedRingBuffer {
 public:
  using Writer = SharedRingBuffer_Writer;
  using Reader = SharedRingBuffer_Reader;

  static constexpr size_t kChunkSize = 256;
  static constexpr size_t kChunkHeaderSize = 4;
  static constexpr size_t kChunkPayloadSize = kChunkSize - kChunkHeaderSize;
  static constexpr size_t kRingBufferHeaderSize = 16;
  static constexpr size_t kPayloadShift = 16;
  static constexpr size_t kFlagsShift = 24;

  enum ChunkFlags : uint8_t {
    kFlagAcquiredForWriting = 1 << 0,
    kFlagContinuesOnNextChunk = 1 << 1,
    kFlagContinuesFromPrevChunk = 1 << 2,
    kFlagDataLoss = 1 << 3,
    kFlagNeedsRewrite = 1 << 4,
  };

  struct alignas(8) RingBufferHeader {
    // TODO spread apart / alignas to avoid cache bouncing? But reason on the
    // code maybe they are frequently accessed together and it's worse.
    std::atomic<uint32_t> wr_off;
    std::atomic<uint32_t> rd_off;
    std::atomic<uint32_t> data_losses;
    std::atomic<uint32_t> futex;
  };
  static_assert(sizeof(RingBufferHeader) == kRingBufferHeaderSize, "");

  struct ChunkHeader {
    // Layout: [flags:8][payload_size:8][writer_id:16]
    static constexpr uint32_t Pack(WriterID id, uint8_t size, uint8_t flags) {
      return static_cast<uint32_t>(id) |
             (static_cast<uint32_t>(size) << kPayloadShift) |
             (static_cast<uint32_t>(flags) << kFlagsShift);
    }
    static constexpr WriterID GetWriterID(uint32_t p) {
      return static_cast<WriterID>(p & 0xFFFF);
    }
    static constexpr uint8_t GetPayloadSize(uint32_t p) {
      return static_cast<uint8_t>((p >> kPayloadShift) & 0xFF);
    }
    static constexpr uint8_t GetFlags(uint32_t p) {
      return static_cast<uint8_t>((p >> kFlagsShift) & 0xFF);
    }
  };

  SharedRingBuffer() = default;
  SharedRingBuffer(void* start, size_t size);

  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;

  Writer CreateWriter(WriterID writer_id);

  uint8_t* start() const { return start_; }
  size_t size() const { return size_; }
  size_t num_chunks() const { return num_chunks_; }
  bool is_valid() const { return start_ != nullptr && num_chunks_ > 0; }

  RingBufferHeader* header() {
    return reinterpret_cast<RingBufferHeader*>(start_);
  }

  std::atomic<uint32_t>* chunk_header_atomic(uint8_t* chunk) {
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk);
  }

  uint8_t* chunk_at(size_t idx) {
    PERFETTO_DCHECK(idx < num_chunks_);
    return start_ + kRingBufferHeaderSize + idx * kChunkSize;
  }

  void IncrementDataLosses() {
    auto* hdr = header();
    uint32_t old_val = hdr->data_losses.load(std::memory_order_relaxed);
    while (old_val < UINT32_MAX) {
      if (hdr->data_losses.compare_exchange_weak(old_val, old_val + 1,
                                                 std::memory_order_relaxed)) {
        break;
      }
    }
  }

 private:
  friend class SharedRingBuffer_Writer;
  friend class SharedRingBuffer_Reader;

  uint8_t* start_ = nullptr;
  size_t size_ = 0;
  size_t num_chunks_ = 0;
};

// Writer for the SharedRingBuffer. Move-only.
class SharedRingBuffer_Writer {
 public:
  SharedRingBuffer_Writer();
  SharedRingBuffer_Writer(SharedRingBuffer* rb, WriterID writer_id);
  ~SharedRingBuffer_Writer() = default;

  SharedRingBuffer_Writer(const SharedRingBuffer_Writer&) = delete;
  SharedRingBuffer_Writer& operator=(const SharedRingBuffer_Writer&) = delete;

  SharedRingBuffer_Writer(SharedRingBuffer_Writer&& other) noexcept;
  SharedRingBuffer_Writer& operator=(SharedRingBuffer_Writer&& other) noexcept;

  bool is_valid() const { return rb_ != nullptr; }
  bool is_writing() const {
    return (SharedRingBuffer::ChunkHeader::GetFlags(cached_header_) &
            SharedRingBuffer::kFlagAcquiredForWriting) != 0;
  }

  void BeginWrite() { BeginWriteInternal(/*extra_flags=*/0); }
  void EndWrite() { EndWriteInternal(/*extra_flags=*/0); }

  // --- Inline proto encoding methods (hot path) ---

  template <typename T>
  void AppendVarInt(uint32_t field_id, T value) {
    uint8_t buf[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* p = buf;
    p = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagVarInt(field_id), p);
    p = protozero::proto_utils::WriteVarInt(value, p);
    WriteBytes(buf, static_cast<size_t>(p - buf));
  }

  template <typename T>
  void AppendSignedVarInt(uint32_t field_id, T value) {
    AppendVarInt(field_id, protozero::proto_utils::ZigZagEncode(value));
  }

  void AppendTinyVarInt(uint32_t field_id, int32_t value) {
    PERFETTO_DCHECK(0 <= value && value < 0x80);
    uint8_t buf[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* p = buf;
    p = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagVarInt(field_id), p);
    *p++ = static_cast<uint8_t>(value);
    WriteBytes(buf, static_cast<size_t>(p - buf));
  }

  template <typename T>
  void AppendFixed(uint32_t field_id, T value) {
    uint8_t buf[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* p = buf;
    p = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagFixed<T>(field_id), p);
    memcpy(p, &value, sizeof(T));
    p += sizeof(T);
    WriteBytes(buf, static_cast<size_t>(p - buf));
  }

  void AppendBytes(uint32_t field_id, const void* data, size_t len) {
    uint8_t buf[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
    uint8_t* p = buf;
    p = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagLengthDelimited(field_id), p);
    p = protozero::proto_utils::WriteVarInt(len, p);
    WriteBytes(buf, static_cast<size_t>(p - buf));
    WriteBytes(data, len);
  }

  void AppendString(uint32_t field_id, const char* str) {
    AppendBytes(field_id, str, strlen(str));
  }

  void AppendString(uint32_t field_id, const char* str, size_t len) {
    AppendBytes(field_id, str, len);
  }

  void AppendString(uint32_t field_id, std::string_view sv) {
    AppendBytes(field_id, sv.data(), sv.size());
  }

  void BeginNestedMessage(uint32_t field_id) {
    uint8_t buf[protozero::proto_utils::kMaxTagEncodedSize];
    uint8_t* p = buf;
    uint32_t tag =
        (field_id << 3) |
        static_cast<uint32_t>(protozero::proto_utils::ProtoWireType::kSGroup);
    p = protozero::proto_utils::WriteVarInt(tag, p);
    WriteBytes(buf, static_cast<size_t>(p - buf));
  }

  void EndNestedMessage() { WriteByte(0x04); }

  PERFETTO_ALWAYS_INLINE void WriteBytes(const void* data, size_t len) {
    PERFETTO_DCHECK(is_writing());
    if (PERFETTO_LIKELY(len <= payload_avail())) {
      return WriteBytesUnchecked(data, len);
    }
    WriteBytesSlow(data, len);
  }

  PERFETTO_ALWAYS_INLINE void WriteByte(uint8_t byte) {
    if (PERFETTO_LIKELY(payload_avail() > 0)) {
      last_chunk_[write_off_++] = byte;
      return;
    }
    WriteBytesSlow(&byte, 1);
  }

 private:
  friend class SharedRingBuffer;

  uint8_t* payload_start() const {
    PERFETTO_DCHECK(last_chunk_);
    return last_chunk_ + SharedRingBuffer::kChunkHeaderSize;
  }

  size_t payload_avail() const {
    return SharedRingBuffer::kChunkPayloadSize - write_off_;
  }

  uint8_t* invalid_chunk() {
    return reinterpret_cast<uint8_t*>(&invalid_chunk_header_);
  }

  PERFETTO_ALWAYS_INLINE void WriteBytesUnchecked(const void* data,
                                                  size_t len) {
    PERFETTO_DCHECK(write_off_ <= SharedRingBuffer::kChunkPayloadSize);
    PERFETTO_DCHECK(len <= payload_avail());
    memcpy(payload_start() + write_off_, data, len);
    write_off_ += len;
  }

  void WriteBytesSlow(const void* data, size_t len);

  void BeginWriteInternal(uint8_t extra_flags);
  void EndWriteInternal(uint8_t extra_flags);
  bool AcquireNewChunk(uint8_t extra_flags);

  SharedRingBuffer* rb_ = nullptr;
  WriterID writer_id_ = 0;
  uint8_t* last_chunk_ = reinterpret_cast<uint8_t*>(&invalid_chunk_header_);
  uint32_t cached_header_ = 0;

  // The two offsets below are relative to the chunk payload (i.e. they do NOT
  // keep the header into account). We need it this way otherwise we cannot
  // express the size=256 case (payload full) with a uint8_t.
  uint8_t write_off_ = 0;
  uint8_t fragment_size_off_ = 0;

  // When adding more state remember to update the move operators.

  // Invalid chunk header. Used for initial "no chunk" state.
  // CAS to acquire always fails, forcing acquisition of a real chunk.
  uint32_t invalid_chunk_header_ =
      SharedRingBuffer::ChunkHeader::Pack(0, 0xFF, 0xFF);

  // Bankruptcy chunk for writes when acquire fails. Treated like a real chunk
  // with a proper header we maintain. We continue writing to it across
  // BeginWrite/EndWrite cycles until it overflows, then retry real acquire.
  alignas(4) uint8_t bankruptcy_chunk_[SharedRingBuffer::kChunkSize] = {};
};

// Single-consumer reader for the SharedRingBuffer.
// Reads chunks from the ring buffer, reassembles fragmented messages, and
// accumulates completed messages in a local buffer.
class SharedRingBuffer_Reader {
 public:
  struct CompletedMessage {
    WriterID writer_id;
    std::string data;
  };

  SharedRingBuffer_Reader();
  explicit SharedRingBuffer_Reader(SharedRingBuffer* rb);
  ~SharedRingBuffer_Reader() = default;

  SharedRingBuffer_Reader(const SharedRingBuffer_Reader&) = delete;
  SharedRingBuffer_Reader& operator=(const SharedRingBuffer_Reader&) = delete;

  SharedRingBuffer_Reader(SharedRingBuffer_Reader&&) noexcept;
  SharedRingBuffer_Reader& operator=(SharedRingBuffer_Reader&&) noexcept;

  bool is_valid() const { return rb_ != nullptr; }

  // Attempts to read one chunk from the ring buffer.
  // Returns true if a chunk was successfully read (may or may not produce
  // complete messages). Returns false if no readable chunks are available
  // (buffer empty or next chunk still being written).
  bool ReadOneChunk();

  // Access completed messages accumulated so far.
  const std::vector<CompletedMessage>& completed_messages() const {
    return completed_messages_;
  }

  // Takes ownership of completed messages, clearing the internal buffer.
  std::vector<CompletedMessage> TakeCompletedMessages() {
    return std::move(completed_messages_);
  }

  void ClearCompletedMessages() { completed_messages_.clear(); }

 private:
  struct WriterState {
    std::string pending_data;
  };

  void ProcessChunkPayload(const uint8_t* payload,
                           uint8_t payload_size,
                           uint32_t header);

  SharedRingBuffer* rb_ = nullptr;

  // TODO think about purging this every now and then.
  base::FlatHashMap<WriterID, WriterState> writer_states_;
  std::vector<CompletedMessage> completed_messages_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_H_
