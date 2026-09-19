/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_PB_PACKED_H_
#define INCLUDE_PERFETTO_PUBLIC_PB_PACKED_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/pb_utils.h"

// Use this API to write a packed repeated field one value at a time. It
// buffers the values so that their length is known before the field is
// written. This supports both length-delimited and append-only proto group
// streams. It works like protozero::PackedVarInt in C++.
// - The begin accessor prepares the PerfettoPbPackedMsg buffer.
// - Each append function adds one value to the buffer.
// - The end accessor writes the field and releases the heap storage.
//
// Rules:
// - Call the end accessor before you finalize the destination message.
//   Finalization does not flush packed buffers.
// - The destination message can take other fields between begin and end, but
//   the normal nesting rules apply. Close any nested message that is open on
//   the destination message before the end accessor writes the packed field.
// - To abandon a field without writing it, call PerfettoPbPackedBufferReset().
// - Do not copy or move an active buffer.

// ***
// Sample usage of PerfettoPbPackedMsg*
// ***
// ```
// struct PerfettoPbPackedMsgUint64 f;
// PROTO_begin_FIELD_NAME(&msg, &f);
// PerfettoPbPackedMsgUint64Append(&f, 1);
// PerfettoPbPackedMsgUint64Append(&f, 2);
// PROTO_end_FIELD_NAME(&msg, &f);
// ```

// ***
// The buffer behind every PerfettoPbPackedMsg type.
// ***

// Size of the inline buffer. Small fields need no heap allocation. A larger
// size grows every PerfettoPbPackedMsg type, and callers keep those on the
// stack.
#define PERFETTO_I_PB_PACKED_INLINE_SIZE 64

// Largest packed value that protozero decoders accept, the same as
// protozero::proto_utils::kMaxMessageLength. Decoders skip larger fields.
#define PERFETTO_I_PB_PACKED_MAX_SIZE \
  ((1u << (7 * PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE)) - 1u)

// The values of one packed field, until the end accessor writes them. Like
// protozero::PackedBufferBase in C++.
// - `begin` points to `inline_buf` while the values fit. After that, it points
//   to a heap block.
// - `write_ptr` points after the last value.
struct PerfettoPbPackedBuffer {
  uint8_t* begin;
  uint8_t* write_ptr;
  uint8_t* end;
  uint8_t inline_buf[PERFETTO_I_PB_PACKED_INLINE_SIZE];
};

// Initializes uninitialized storage. To clear a buffer in use, call
// PerfettoPbPackedBufferReset() instead. Init does not free the heap block.
static inline void PerfettoPbPackedBufferInit(
    struct PerfettoPbPackedBuffer* buf) {
  buf->begin = buf->inline_buf;
  buf->write_ptr = buf->inline_buf;
  buf->end = buf->inline_buf + sizeof(buf->inline_buf);
}

// Makes room for `size` more bytes.
// - A fixed-width value reserves its width. A varint reserves 10 bytes, its
//   largest size, because it is written in place before its size is known.
// - If the room is missing, the capacity doubles. One doubling is enough,
//   because `size` is at most 10 and the capacity is at least
//   PERFETTO_I_PB_PACKED_INLINE_SIZE.
// - The capacity stops at PERFETTO_I_PB_PACKED_MAX_SIZE. Aborts if the field
//   needs more, or if the allocation fails.
static inline void PerfettoPbPackedBufferGrowIfNeeded(
    struct PerfettoPbPackedBuffer* buf,
    size_t size) {
  size_t used = PERFETTO_STATIC_CAST(size_t, buf->write_ptr - buf->begin);
  size_t capacity = PERFETTO_STATIC_CAST(size_t, buf->end - buf->begin);
  uint8_t* block;

  if (PERFETTO_LIKELY(capacity - used >= size))
    return;

  if (PERFETTO_UNLIKELY(used + size > PERFETTO_I_PB_PACKED_MAX_SIZE))
    abort();

  // Double the capacity, up to the size that decoders accept.
  capacity *= 2;
  if (capacity > PERFETTO_I_PB_PACKED_MAX_SIZE)
    capacity = PERFETTO_I_PB_PACKED_MAX_SIZE;

  // Move the values to a larger heap block. The first growth leaves the inline
  // buffer, so it copies them. Later growths resize the heap block.
  if (buf->begin == buf->inline_buf) {
    block = PERFETTO_STATIC_CAST(uint8_t*, malloc(capacity));
    if (block)
      memcpy(block, buf->inline_buf, used);
  } else {
    block = PERFETTO_STATIC_CAST(uint8_t*, realloc(buf->begin, capacity));
  }
  if (PERFETTO_UNLIKELY(!block))
    abort();

  buf->begin = block;
  buf->write_ptr = block + used;
  buf->end = block + capacity;
}

static inline void PerfettoPbPackedBufferAppendVarInt(
    struct PerfettoPbPackedBuffer* buf,
    uint64_t value) {
  PerfettoPbPackedBufferGrowIfNeeded(buf, PERFETTO_PB_VARINT_MAX_SIZE_64);
  buf->write_ptr = PerfettoPbWriteVarInt(value, buf->write_ptr);
}

static inline void PerfettoPbPackedBufferAppendFixed32(
    struct PerfettoPbPackedBuffer* buf,
    uint32_t value) {
  PerfettoPbPackedBufferGrowIfNeeded(buf, sizeof(value));
  buf->write_ptr = PerfettoPbWriteFixed32(value, buf->write_ptr);
}

static inline void PerfettoPbPackedBufferAppendFixed64(
    struct PerfettoPbPackedBuffer* buf,
    uint64_t value) {
  PerfettoPbPackedBufferGrowIfNeeded(buf, sizeof(value));
  buf->write_ptr = PerfettoPbWriteFixed64(value, buf->write_ptr);
}

// Writes the values in `buf` to `msg` as the packed field `field_id`. It does
// not release the heap storage of `buf`. See PerfettoPbPackedBufferReset().
static inline void PerfettoPbMsgAppendPackedField(
    struct PerfettoPbMsg* msg,
    int32_t field_id,
    const struct PerfettoPbPackedBuffer* buf) {
  PerfettoPbMsgAppendType2Field(
      msg, field_id, buf->begin,
      PERFETTO_STATIC_CAST(size_t, buf->write_ptr - buf->begin));
}

// Releases the heap storage of `buf`, if there is one, and leaves `buf` empty.
// The end accessor calls it after it writes the field. Call it directly to
// abandon a field.
static inline void PerfettoPbPackedBufferReset(
    struct PerfettoPbPackedBuffer* buf) {
  if (buf->begin != buf->inline_buf)
    free(buf->begin);
  PerfettoPbPackedBufferInit(buf);
}

// ***
// Implementations of struct PerfettoPbPackedMsg for all supported field types.
// ***
struct PerfettoPbPackedMsgUint64 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgUint64Append(
    struct PerfettoPbPackedMsgUint64* packed,
    uint64_t value) {
  PerfettoPbPackedBufferAppendVarInt(&packed->buf, value);
}

struct PerfettoPbPackedMsgUint32 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgUint32Append(
    struct PerfettoPbPackedMsgUint32* packed,
    uint32_t value) {
  PerfettoPbPackedBufferAppendVarInt(&packed->buf, value);
}

struct PerfettoPbPackedMsgInt64 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgInt64Append(
    struct PerfettoPbPackedMsgInt64* packed,
    int64_t value) {
  PerfettoPbPackedBufferAppendVarInt(&packed->buf,
                                     PERFETTO_STATIC_CAST(uint64_t, value));
}

struct PerfettoPbPackedMsgInt32 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgInt32Append(
    struct PerfettoPbPackedMsgInt32* packed,
    int32_t value) {
  PerfettoPbPackedBufferAppendVarInt(&packed->buf,
                                     PERFETTO_STATIC_CAST(uint64_t, value));
}

struct PerfettoPbPackedMsgSint64 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgSint64Append(
    struct PerfettoPbPackedMsgSint64* packed,
    int64_t value) {
  uint64_t encoded = PerfettoPbZigZagEncode64(value);
  PerfettoPbPackedBufferAppendVarInt(&packed->buf, encoded);
}

struct PerfettoPbPackedMsgSint32 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgSint32Append(
    struct PerfettoPbPackedMsgSint32* packed,
    int32_t value) {
  uint64_t encoded =
      PerfettoPbZigZagEncode64(PERFETTO_STATIC_CAST(int64_t, value));
  PerfettoPbPackedBufferAppendVarInt(&packed->buf, encoded);
}

struct PerfettoPbPackedMsgFixed64 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgFixed64Append(
    struct PerfettoPbPackedMsgFixed64* packed,
    uint64_t value) {
  PerfettoPbPackedBufferAppendFixed64(&packed->buf, value);
}

struct PerfettoPbPackedMsgFixed32 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgFixed32Append(
    struct PerfettoPbPackedMsgFixed32* packed,
    uint32_t value) {
  PerfettoPbPackedBufferAppendFixed32(&packed->buf, value);
}

struct PerfettoPbPackedMsgSfixed64 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgSfixed64Append(
    struct PerfettoPbPackedMsgSfixed64* packed,
    int64_t value) {
  uint64_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbPackedBufferAppendFixed64(&packed->buf, encoded);
}

struct PerfettoPbPackedMsgSfixed32 {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgSfixed32Append(
    struct PerfettoPbPackedMsgSfixed32* packed,
    int32_t value) {
  uint32_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbPackedBufferAppendFixed32(&packed->buf, encoded);
}

struct PerfettoPbPackedMsgDouble {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgDoubleAppend(
    struct PerfettoPbPackedMsgDouble* packed,
    double value) {
  uint64_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbPackedBufferAppendFixed64(&packed->buf, encoded);
}

struct PerfettoPbPackedMsgFloat {
  struct PerfettoPbPackedBuffer buf;
};
static inline void PerfettoPbPackedMsgFloatAppend(
    struct PerfettoPbPackedMsgFloat* packed,
    float value) {
  uint32_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbPackedBufferAppendFixed32(&packed->buf, encoded);
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_PACKED_H_
