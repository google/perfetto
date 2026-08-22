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
#include <stdint.h>
#include <string.h>

#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/stream_writer.h"

// The number of bytes reserved by this implementation to encode a protobuf type
// 2 field size as var-int. Keep this in sync with kMessageLengthFieldSize in
// proto_utils.h.
#define PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE 4

// Stored in PerfettoPbMsg::size so group mode does not change the public C ABI
// layout. Protozero messages are bounded below 2^28 bytes, leaving this bit
// unavailable to a valid message size.
#define PERFETTO_PB_MSG_GROUP_ENCODING_BIT (1u << 31)
#define PERFETTO_PB_MSG_SIZE_MASK (~PERFETTO_PB_MSG_GROUP_ENCODING_BIT)

// Points to the memory used by a `PerfettoPbMsg` for writing.
struct PerfettoPbMsgWriter {
  struct PerfettoStreamWriter writer;
};

struct PerfettoPbMsg {
  // Pointer to a non-aligned pre-reserved var-int slot of
  // PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE bytes. If not NULL,
  // protozero_length_buf_finalize() will write the size of proto-encoded
  // message in the pointed memory region.
  union {
    uint8_t* size_field;

    // Valid instead of size_field when group encoding is enabled. Full
    // protobuf field ids use 29 bits and therefore fit without truncation.
    uint32_t group_field_id;
  };

  // Current size of the buffer.
  uint32_t size;

  struct PerfettoPbMsgWriter* writer;

  struct PerfettoPbMsg* nested;
  struct PerfettoPbMsg* parent;
};

static inline int PerfettoPbMsgNestedMessagesAreGroups(
    const struct PerfettoPbMsg* msg) {
  return (msg->size & PERFETTO_PB_MSG_GROUP_ENCODING_BIT) != 0;
}

static inline uint32_t PerfettoPbMsgGetSize(const struct PerfettoPbMsg* msg) {
  return msg->size & PERFETTO_PB_MSG_SIZE_MASK;
}

static inline void PerfettoPbMsgInit(struct PerfettoPbMsg* msg,
                                     struct PerfettoPbMsgWriter* writer) {
  msg->size_field = PERFETTO_NULL;
  msg->size = (PerfettoStreamWriterGetSerializationFlags(&writer->writer) &
               PERFETTO_STREAM_WRITER_NESTED_MESSAGES_AS_GROUPS)
                  ? PERFETTO_PB_MSG_GROUP_ENCODING_BIT
                  : 0;
  msg->writer = writer;
  msg->nested = PERFETTO_NULL;
  msg->parent = PERFETTO_NULL;
}

static inline void PerfettoPbMsgPatch(struct PerfettoPbMsg* msg) {
  static_assert(
      PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE == PERFETTO_STREAM_WRITER_PATCH_SIZE,
      "PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE doesn't match patch size");
  assert(!PerfettoPbMsgNestedMessagesAreGroups(msg));
  msg->size_field =
      PerfettoStreamWriterAnnotatePatch(&msg->writer->writer, msg->size_field);
}

static inline void PerfettoPbMsgPatchStack(struct PerfettoPbMsg* msg) {
  uint8_t* const cur_range_end = msg->writer->writer.end;
  uint8_t* const cur_range_begin = msg->writer->writer.begin;
  if (PerfettoPbMsgNestedMessagesAreGroups(msg))
    return;
  while (msg && cur_range_begin <= msg->size_field &&
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
  if (PERFETTO_UNLIKELY(PerfettoPbMsgNestedMessagesAreGroups(parent))) {
    PerfettoPbMsgAppendVarInt(
        parent, PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_START_GROUP));
    nested->group_field_id = PERFETTO_STATIC_CAST(uint32_t, field_id);
    nested->size = PERFETTO_PB_MSG_GROUP_ENCODING_BIT;
    nested->writer = parent->writer;
    nested->nested = PERFETTO_NULL;
    nested->parent = parent;
    parent->nested = nested;
    return;
  }

  PerfettoPbMsgAppendVarInt(
      parent, PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED));

  PerfettoPbMsgInit(nested, parent->writer);
  if (PERFETTO_UNLIKELY(
          PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE >
          PerfettoStreamWriterAvailableBytes(&parent->writer->writer))) {
    PerfettoPbMsgPatchStack(parent);
  }
  nested->size_field = PERFETTO_REINTERPRET_CAST(
      uint8_t*,
      PerfettoStreamWriterReserveBytes(&nested->writer->writer,
                                       PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE));
  nested->parent = parent;
  parent->size += PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE;
  parent->nested = nested;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg);

static inline void PerfettoPbMsgEndNested(struct PerfettoPbMsg* parent) {
  parent->size += PerfettoPbMsgFinalize(parent->nested);
  parent->nested = PERFETTO_NULL;
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg) {
  if (msg->nested)
    PerfettoPbMsgEndNested(msg);

  if (PERFETTO_UNLIKELY(PerfettoPbMsgNestedMessagesAreGroups(msg))) {
    // Zeroing the field id first is what makes a second Finalize() a no-op,
    // the way the not-finalized message states do on the C++ side. A root
    // message has no id and therefore no end tag: its framing belongs to
    // whoever owns the stream.
    if (msg->group_field_id != 0) {
      const int32_t field_id =
          PERFETTO_STATIC_CAST(int32_t, msg->group_field_id);
      msg->group_field_id = 0;
      PerfettoPbMsgAppendVarInt(
          msg, PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_END_GROUP));
    }
    return PerfettoPbMsgGetSize(msg);
  }

  // Write the length of the nested message a posteriori, using a leading-zero
  // redundant varint encoding.
  if (msg->size_field) {
    uint32_t size_to_write;
    size_to_write = PerfettoPbMsgGetSize(msg);
    for (size_t i = 0; i < PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE; i++) {
      const uint8_t msb = (i < 3) ? 0x80 : 0;
      msg->size_field[i] = (size_to_write & 0xFF) | msb;
      size_to_write >>= 7;
    }
    msg->size_field = PERFETTO_NULL;
  }

  return PerfettoPbMsgGetSize(msg);
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_MSG_H_
