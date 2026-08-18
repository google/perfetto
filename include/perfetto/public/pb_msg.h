/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_PB_MSG_H_
#define INCLUDE_PERFETTO_PUBLIC_PB_MSG_H_

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/stream_writer.h"

// The number of bytes reserved by this implementation to encode a protobuf type
// 2 field size as var-int. Keep this in sync with kMessageLengthFieldSize in
// proto_utils.h.
#define PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE 4

// How a message frames its nested messages. Selected once, when the message is
// initialized, and inherited by every nested message. This is the C half of
// protozero::NestedMessageEncoding; the two runtimes emit identical bytes for
// the same call sequence.
enum PerfettoPbMsgEncoding {
  // Ordinary protobuf: a nested message is a wire-type-2 field whose four-byte
  // length is reserved up front and patched when the message is finalized.
  PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED = 0,
  // The private start-tag-and-terminator framing described at
  // PERFETTO_PB_NESTED_MESSAGE_TERMINATOR in pb_utils.h, so that no length is
  // ever backfilled after the bytes in front of it may already be visible to a
  // concurrent reader.
  PERFETTO_PB_MSG_ENCODING_START_TAG_AND_TERMINATOR = 1,
};

// The two high bits of PerfettoPbMsg::size hold state rather than size, and
// only in PERFETTO_PB_MSG_ENCODING_START_TAG_AND_TERMINATOR mode. A protobuf
// message's length is written into four varint bytes, which caps it at 2^28 -
// 1, so borrowing bits 30 and 31 costs no representable size and every ordinary
// `size += n` preserves them.
//
// They live in `size` rather than in a new field because struct PerfettoPbMsg
// is public ABI: old C callers must keep the same layout on both pointer
// widths.
//
// In the length-delimited mode `size` keeps exactly the meaning it has always
// had - the byte count, with no bits set, before and after finalization - so
// that a caller reading the public field directly still reads a size. The
// terminated mode is opt-in through PerfettoPbMsgInitWithEncoding(), and a
// caller that opts in has to use PerfettoPbMsgSize().
#define PERFETTO_PB_MSG_USES_TERMINATOR_BIT UINT32_C(0x80000000)
#define PERFETTO_PB_MSG_TERMINATOR_WRITTEN_BIT UINT32_C(0x40000000)
#define PERFETTO_PB_MSG_SIZE_MASK UINT32_C(0x3fffffff)

// Points to the memory used by a `PerfettoPbMsg` for writing.
struct PerfettoPbMsgWriter {
  struct PerfettoStreamWriter writer;
};

struct PerfettoPbMsg {
  // Pointer to a non-aligned pre-reserved var-int slot of
  // PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE bytes. If not NULL,
  // protozero_length_buf_finalize() will write the size of proto-encoded
  // message in the pointed memory region.
  uint8_t* size_field;

  // Current size of the buffer.
  uint32_t size;

  struct PerfettoPbMsgWriter* writer;

  struct PerfettoPbMsg* nested;
  struct PerfettoPbMsg* parent;
};

// The number of bytes written into |msg| so far, with the state bits masked
// off. Every read of PerfettoPbMsg::size inside this header goes through this;
// in the length-delimited mode it is the raw field, because no bit is ever set
// there.
static inline uint32_t PerfettoPbMsgSize(const struct PerfettoPbMsg* msg) {
  return msg->size & PERFETTO_PB_MSG_SIZE_MASK;
}

static inline bool PerfettoPbMsgUsesTerminator(
    const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_USES_TERMINATOR_BIT) != 0;
}

// Whether the terminator byte has already been emitted for |msg|. Set only in
// the terminated mode, which is the only one that has to remember: a
// length-delimited message is idempotent to finalize without recording
// anything, so this is always false for one.
static inline bool PerfettoPbMsgTerminatorWritten(
    const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_TERMINATOR_WRITTEN_BIT) != 0;
}

static inline void PerfettoPbMsgInitWithEncoding(
    struct PerfettoPbMsg* msg,
    struct PerfettoPbMsgWriter* writer,
    enum PerfettoPbMsgEncoding encoding) {
  msg->size_field = PERFETTO_NULL;
  msg->writer = writer;
  msg->nested = PERFETTO_NULL;
  msg->parent = PERFETTO_NULL;
  if (encoding == PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED) {
    msg->size = 0;
  } else if (encoding == PERFETTO_PB_MSG_ENCODING_START_TAG_AND_TERMINATOR) {
    msg->size = PERFETTO_PB_MSG_USES_TERMINATOR_BIT;
  } else {
    // An encoding this build has never heard of can only come from a newer,
    // independently versioned ABI. Quietly falling back to length-delimited
    // would emit framing the caller's root does not expect, so fail here,
    // before a single byte is written, rather than produce a malformed trace.
    abort();
  }
}

// Canonical length-delimited protobuf, the default for every existing caller.
static inline void PerfettoPbMsgInit(struct PerfettoPbMsg* msg,
                                     struct PerfettoPbMsgWriter* writer) {
  PerfettoPbMsgInitWithEncoding(msg, writer,
                                PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED);
}

static inline void PerfettoPbMsgPatch(struct PerfettoPbMsg* msg) {
  static_assert(
      PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE == PERFETTO_STREAM_WRITER_PATCH_SIZE,
      "PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE doesn't match patch size");
  msg->size_field =
      PerfettoStreamWriterAnnotatePatch(&msg->writer->writer, msg->size_field);
}

static inline void PerfettoPbMsgPatchStack(struct PerfettoPbMsg* msg) {
  uint8_t* const cur_range_end = msg->writer->writer.end;
  uint8_t* const cur_range_begin = msg->writer->writer.begin;
  // The explicit null check is what makes this a no-op in terminated mode,
  // where no message ever reserves a length. It is not redundant with the range
  // test below: comparing a null pointer with < and <= is not a valid way to
  // test for a sentinel, and this path is taken every time the stream writer
  // rolls to another chunk.
  while (msg && msg->size_field && cur_range_begin <= msg->size_field &&
         msg->size_field < cur_range_end) {
    PerfettoPbMsgPatch(msg);
    msg = msg->parent;
  }
}

static inline void PerfettoPbMsgAppendBytes(struct PerfettoPbMsg* msg,
                                            const uint8_t* begin,
                                            size_t size) {
  if (PERFETTO_UNLIKELY(
          size > PerfettoStreamWriterAvailableBytes(&msg->writer->writer))) {
    PerfettoPbMsgPatchStack(msg);
  }
  PerfettoStreamWriterAppendBytes(&msg->writer->writer, begin, size);
  // Plain addition: a legal message size never reaches bit 30, so the state
  // bits above are preserved.
  msg->size += size;
}

static inline void PerfettoPbMsgAppendByte(struct PerfettoPbMsg* msg,
                                           uint8_t value) {
  PerfettoPbMsgAppendBytes(msg, &value, 1);
}

static inline void PerfettoPbMsgAppendVarInt(struct PerfettoPbMsg* msg,
                                             uint64_t value) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_64];
  buf_end = PerfettoPbWriteVarInt(value, buf);

  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));
}

static inline void PerfettoPbMsgAppendFixed64(struct PerfettoPbMsg* msg,
                                              uint64_t value) {
  uint8_t buf[8];
  PerfettoPbWriteFixed64(value, buf);

  PerfettoPbMsgAppendBytes(msg, buf, 8);
}

static inline void PerfettoPbMsgAppendFixed32(struct PerfettoPbMsg* msg,
                                              uint32_t value) {
  uint8_t buf[4];
  PerfettoPbWriteFixed32(value, buf);

  PerfettoPbMsgAppendBytes(msg, buf, 4);
}

static inline void PerfettoPbMsgAppendType0Field(struct PerfettoPbMsg* msg,
                                                 int32_t field_id,
                                                 uint64_t value) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_64 + PERFETTO_PB_VARINT_MAX_SIZE_32];
  buf_end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_VARINT), buf);
  buf_end = PerfettoPbWriteVarInt(value, buf_end);

  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));
}

static inline void PerfettoPbMsgAppendType2Field(struct PerfettoPbMsg* msg,
                                                 int32_t field_id,
                                                 const uint8_t* data,
                                                 size_t size) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_64 + PERFETTO_PB_VARINT_MAX_SIZE_32];
  buf_end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED), buf);
  buf_end =
      PerfettoPbWriteVarInt(PERFETTO_STATIC_CAST(uint64_t, size), buf_end);
  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));

  PerfettoPbMsgAppendBytes(msg, data, size);
}

static inline void PerfettoPbMsgAppendFixed32Field(struct PerfettoPbMsg* msg,
                                                   int32_t field_id,
                                                   uint32_t value) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_32 + 4];
  buf_end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_FIXED32), buf);
  buf_end = PerfettoPbWriteFixed32(value, buf_end);

  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));
}

static inline void PerfettoPbMsgAppendFloatField(struct PerfettoPbMsg* msg,
                                                 int32_t field_id,
                                                 float value) {
  uint32_t val;
  memcpy(&val, &value, sizeof val);
  PerfettoPbMsgAppendFixed32Field(msg, field_id, val);
}

static inline void PerfettoPbMsgAppendFixed64Field(struct PerfettoPbMsg* msg,
                                                   int32_t field_id,
                                                   uint64_t value) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_32 + 8];
  buf_end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_FIXED64), buf);
  buf_end = PerfettoPbWriteFixed64(value, buf_end);

  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));
}

static inline void PerfettoPbMsgAppendDoubleField(struct PerfettoPbMsg* msg,
                                                  int32_t field_id,
                                                  double value) {
  uint64_t val;
  memcpy(&val, &value, sizeof val);
  PerfettoPbMsgAppendFixed64Field(msg, field_id, val);
}

static inline void PerfettoPbMsgAppendCStrField(struct PerfettoPbMsg* msg,
                                                int32_t field_id,
                                                const char* c_str) {
  PerfettoPbMsgAppendType2Field(
      msg, field_id, PERFETTO_REINTERPRET_CAST(const uint8_t*, c_str),
      strlen(c_str));
}

static inline void PerfettoPbMsgBeginNested(struct PerfettoPbMsg* parent,
                                            struct PerfettoPbMsg* nested,
                                            int32_t field_id) {
  // In terminated mode the wire-type-3 tag is only the opening marker of the
  // private framing; the field id is not needed again to close the message.
  const bool terminated = PerfettoPbMsgUsesTerminator(parent);
  if (terminated) {
    PerfettoPbMsgAppendVarInt(parent, PerfettoPbMakeStartGroupTag(field_id));
    PerfettoPbMsgInitWithEncoding(
        nested, parent->writer,
        PERFETTO_PB_MSG_ENCODING_START_TAG_AND_TERMINATOR);
  } else {
    PerfettoPbMsgAppendVarInt(
        parent, PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED));
    PerfettoPbMsgInitWithEncoding(nested, parent->writer,
                                  PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED);
    if (PERFETTO_UNLIKELY(
            PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE >
            PerfettoStreamWriterAvailableBytes(&parent->writer->writer))) {
      PerfettoPbMsgPatchStack(parent);
    }
    nested->size_field = PERFETTO_REINTERPRET_CAST(
        uint8_t*,
        PerfettoStreamWriterReserveBytes(&nested->writer->writer,
                                         PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE));
    parent->size += PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE;
  }
  nested->parent = parent;
  parent->nested = nested;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg);

static inline void PerfettoPbMsgEndNested(struct PerfettoPbMsg* parent) {
  parent->size += PerfettoPbMsgFinalize(parent->nested);
  parent->nested = PERFETTO_NULL;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg) {
  if (PERFETTO_UNLIKELY(PerfettoPbMsgUsesTerminator(msg))) {
    // Finalizing twice must not emit a second terminator byte, and clearing
    // size_field is not available here because a terminated message never
    // reserves one. So this mode - and only this mode - records that it is
    // closed.
    if (PerfettoPbMsgTerminatorWritten(msg))
      return PerfettoPbMsgSize(msg);

    if (msg->nested)
      PerfettoPbMsgEndNested(msg);

    // A nested message closes with the single private terminator byte. A root
    // has no parent, and its framing belongs to whoever owns the stream, so it
    // closes with nothing.
    if (msg->parent) {
      PerfettoPbMsgAppendByte(
          msg,
          PERFETTO_STATIC_CAST(uint8_t, PERFETTO_PB_NESTED_MESSAGE_TERMINATOR));
    }
    msg->size |= PERFETTO_PB_MSG_TERMINATOR_WRITTEN_BIT;
    return PerfettoPbMsgSize(msg);
  }

  // Length-delimited, unchanged from before this mode existed, down to leaving
  // `size` as a plain byte count. Clearing size_field is what makes a second
  // call a no-op, so there is nothing to remember.
  if (msg->nested)
    PerfettoPbMsgEndNested(msg);

  // Write the length of the nested message a posteriori, using a leading-zero
  // redundant varint encoding.
  if (msg->size_field) {
    uint32_t size_to_write;
    size_to_write = msg->size;
    for (size_t i = 0; i < PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE; i++) {
      const uint8_t msb = (i < 3) ? 0x80 : 0;
      msg->size_field[i] = (size_to_write & 0xFF) | msb;
      size_to_write >>= 7;
    }
    msg->size_field = PERFETTO_NULL;
  }

  return msg->size;
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_MSG_H_
