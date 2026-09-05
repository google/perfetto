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
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/stream_writer.h"

// The number of bytes reserved by this implementation to encode a protobuf type
// 2 field size as var-int. Keep this in sync with kMessageLengthFieldSize in
// proto_utils.h.
#define PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE 4

// Encoding used for nested messages. The root chooses it when it is initialized
// and all its children use the same encoding.
enum PerfettoPbMsgEncoding {
  // Normal protobuf. The nested-message length is filled in on finalization.
  PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED = 0,

  // Append-only proto-group encoding used by tracing v2. See
  // PERFETTO_PB_PROTO_GROUP_END_BYTE in pb_utils.h.
  PERFETTO_PB_MSG_ENCODING_PROTO_GROUP = 1,
};

// PerfettoPbMsg is public ABI and cannot grow. A four-byte length uses 28 bits,
// leaving the top three bits of `size` available for proto-group state.
//
//    31    30    29   28                                        0
//   +-----+-----+-----+------------------------------------------+
//   | PG  | FIN | BUF |            number of bytes written       |
//   +-----+-----+-----+------------------------------------------+
//   PG: proto-group encoding
//   FIN: Finalize() already ran
//   BUF: current STRING/PACKED field is heap-buffered
#define PERFETTO_PB_MSG_PROTO_GROUP_BIT (UINT32_C(1) << 31)
#define PERFETTO_PB_MSG_FINALIZED_BIT (UINT32_C(1) << 30)
#define PERFETTO_PB_MSG_BUFFERED_BIT (UINT32_C(1) << 29)
#define PERFETTO_PB_MSG_SIZE_MASK ((UINT32_C(1) << 29) - 1)

static_assert(PERFETTO_PB_MSG_SIZE_MASK >=
                  (UINT32_C(1) << (PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE * 7)) -
                      1,
              "The size bits must cover every length the four-byte "
              "nested-message length field can hold");

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

  // Number of bytes written. In proto-group mode the top three bits are
  // reserved as described above; use PerfettoPbMsgSize() to read the byte
  // count.
  uint32_t size;

  struct PerfettoPbMsgWriter* writer;

  struct PerfettoPbMsg* nested;
  struct PerfettoPbMsg* parent;
};

// Proto-group STRING and PACKED fields are buffered until their length is
// known. `PerfettoPbMsg::writer` stores only a pointer to the embedded
// `msg_writer`. Finalization casts that pointer back to the owning buffered
// field to access `heap_buffer` and `field_id`, so `msg_writer` must be at
// offset zero.
struct PerfettoPbMsgBufferedField {
  struct PerfettoPbMsgWriter msg_writer;
  struct PerfettoHeapBuffer* heap_buffer;
  int32_t field_id;
};

static_assert(offsetof(struct PerfettoPbMsgBufferedField, msg_writer) == 0,
              "PerfettoPbMsg::writer is cast back to the enclosing "
              "PerfettoPbMsgBufferedField");

// Returns the number of bytes written into |msg|.
static inline uint32_t PerfettoPbMsgSize(const struct PerfettoPbMsg* msg) {
  return msg->size & PERFETTO_PB_MSG_SIZE_MASK;
}

static inline bool PerfettoPbMsgUsesProtoGroup(
    const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_PROTO_GROUP_BIT) != 0;
}

static inline bool PerfettoPbMsgIsBuffered(const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_BUFFERED_BIT) != 0;
}

static inline bool PerfettoPbMsgIsFinalized(const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_FINALIZED_BIT) != 0;
}

static inline void PerfettoPbMsgInitWithEncoding(
    struct PerfettoPbMsg* msg,
    struct PerfettoPbMsgWriter* writer,
    enum PerfettoPbMsgEncoding encoding) {
  msg->size_field = PERFETTO_NULL;
  msg->writer = writer;
  msg->nested = PERFETTO_NULL;
  msg->parent = PERFETTO_NULL;
  switch (encoding) {
    case PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED:
      msg->size = 0;
      return;
    case PERFETTO_PB_MSG_ENCODING_PROTO_GROUP:
      msg->size = PERFETTO_PB_MSG_PROTO_GROUP_BIT;
      return;
  }

  // The root and its children must use the same encoding.
  abort();
}

// Existing callers continue to write normal length-delimited protobuf.
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
  // A proto-group message has no length field to patch, so the walk stops at
  // the first message without one.
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

static inline void PerfettoPbMsgAppendType2FieldHeader(
    struct PerfettoPbMsg* msg,
    int32_t field_id,
    size_t size) {
  uint8_t* buf_end;
  uint8_t buf[PERFETTO_PB_VARINT_MAX_SIZE_64 + PERFETTO_PB_VARINT_MAX_SIZE_32];
  buf_end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED), buf);
  buf_end =
      PerfettoPbWriteVarInt(PERFETTO_STATIC_CAST(uint64_t, size), buf_end);
  PerfettoPbMsgAppendBytes(msg, buf,
                           PERFETTO_STATIC_CAST(size_t, buf_end - buf));
}

static inline void PerfettoPbMsgAppendType2Field(struct PerfettoPbMsg* msg,
                                                 int32_t field_id,
                                                 const uint8_t* data,
                                                 size_t size) {
  PerfettoPbMsgAppendType2FieldHeader(msg, field_id, size);

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
  const bool use_proto_group = PerfettoPbMsgUsesProtoGroup(parent);
  if (PERFETTO_UNLIKELY(use_proto_group)) {
    PerfettoPbMsgAppendVarInt(parent, PerfettoPbMakeStartGroupTag(field_id));
    PerfettoPbMsgInitWithEncoding(nested, parent->writer,
                                  PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
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

// Starts a piecewise STRING or PACKED field, whose length is not yet known.
// Length-delimited messages patch a reservation; proto-group messages cannot
// patch and therefore buffer the field until it closes.
static inline void PerfettoPbMsgBeginLengthDelimitedField(
    struct PerfettoPbMsg* parent,
    struct PerfettoPbMsg* nested,
    int32_t field_id) {
  struct PerfettoPbMsgBufferedField* buffered_field;

  if (PERFETTO_LIKELY(!PerfettoPbMsgUsesProtoGroup(parent))) {
    PerfettoPbMsgBeginNested(parent, nested, field_id);
    return;
  }

  buffered_field =
      PERFETTO_STATIC_CAST(struct PerfettoPbMsgBufferedField*,
                           malloc(sizeof(struct PerfettoPbMsgBufferedField)));
  if (PERFETTO_UNLIKELY(!buffered_field))
    abort();
  buffered_field->heap_buffer =
      PerfettoHeapBufferCreate(&buffered_field->msg_writer.writer);
  buffered_field->field_id = field_id;

  PerfettoPbMsgInitWithEncoding(nested, &buffered_field->msg_writer,
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  nested->size |= PERFETTO_PB_MSG_BUFFERED_BIT;
  nested->parent = parent;
  parent->nested = nested;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg);

static inline void PerfettoPbMsgEndNested(struct PerfettoPbMsg* parent) {
  parent->size += PerfettoPbMsgFinalize(parent->nested);
  parent->nested = PERFETTO_NULL;
}

// Emits and frees a buffered proto-group field; returns bytes added to parent.
static inline size_t PerfettoPbMsgFinalizeBufferedField(
    struct PerfettoPbMsg* msg) {
  uint32_t payload_size;
  struct PerfettoPbMsgBufferedField* buffered_field;

  if (PerfettoPbMsgIsFinalized(msg))
    return PerfettoPbMsgSize(msg);

  if (msg->nested)
    PerfettoPbMsgEndNested(msg);

  assert(msg->parent && msg->parent->nested == msg);
  payload_size = PerfettoPbMsgSize(msg);
  buffered_field = PERFETTO_REINTERPRET_CAST(struct PerfettoPbMsgBufferedField*,
                                             msg->writer);
  PerfettoPbMsgAppendType2FieldHeader(msg->parent, buffered_field->field_id,
                                      payload_size);
  PerfettoHeapBufferAppendToStream(buffered_field->heap_buffer,
                                   &buffered_field->msg_writer.writer,
                                   &msg->parent->writer->writer);
  PerfettoHeapBufferDestroy(buffered_field->heap_buffer,
                            &buffered_field->msg_writer.writer);
  free(buffered_field);

  msg->writer = PERFETTO_NULL;
  msg->size |= PERFETTO_PB_MSG_FINALIZED_BIT;
  return payload_size;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg) {
  if (PERFETTO_UNLIKELY(PerfettoPbMsgIsBuffered(msg)))
    return PerfettoPbMsgFinalizeBufferedField(msg);

  if (PERFETTO_UNLIKELY(PerfettoPbMsgUsesProtoGroup(msg))) {
    if (PerfettoPbMsgIsFinalized(msg))
      return PerfettoPbMsgSize(msg);

    if (msg->nested)
      PerfettoPbMsgEndNested(msg);

    // Only nested messages have a closing byte. The root has no wrapper.
    if (msg->parent) {
      PerfettoPbMsgAppendByte(
          msg, PERFETTO_STATIC_CAST(uint8_t, PERFETTO_PB_PROTO_GROUP_END_BYTE));
    }
    msg->size |= PERFETTO_PB_MSG_FINALIZED_BIT;
    return PerfettoPbMsgSize(msg);
  }

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
