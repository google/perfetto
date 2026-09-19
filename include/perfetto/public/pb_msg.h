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

#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/stream_writer.h"

// The number of bytes reserved by this implementation to encode a protobuf type
// 2 field size as var-int. Keep this in sync with kMessageLengthFieldSize in
// proto_utils.h.
#define PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE 4

// Encoding for nested messages. The caller selects it when it initializes the
// root. Each child inherits its parent's encoding.
// Keep in sync with protozero::Message::Encoding.
enum PerfettoPbMsgEncoding {
  // Standard protobuf. Finalization writes each child's length into a reserved
  // field before the child's contents.
  PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED = 0,

  // Append-only proto group encoding used by tracing v2.
  //
  // - Supports scalars, whole string/bytes/packed values and nested messages.
  // - Incremental STRING/PACKED accessors stage the value until the field
  //   closes. They then write the length and value to the output.
  // - Incremental bytes fields can contain serialized protobuf messages.
  //   Opening a nested message moves the payload to heap storage.
  //
  // See PERFETTO_PB_PROTO_GROUP_END_BYTE in pb_utils.h for the wire format.
  PERFETTO_PB_MSG_ENCODING_PROTO_GROUP = 1,

  // Internal encoding for a staged STRING/PACKED value on a proto group
  // stream.
  //
  // - Only PerfettoPbMsgBeginStaged() sets this encoding.
  // - PerfettoPbMsgInitWithEncoding() aborts if given this encoding.
  PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED = 2,
};

// Points to the memory used by a `PerfettoPbMsg` for writing.
struct PerfettoPbMsgWriter {
  struct PerfettoStreamWriter writer;
};

// Storage for a STRING or PACKED value on a proto group stream. The value
// stays here until the field closes. See PerfettoPbMsgBeginStaged().
//
// - Keep `writer` first. A pointer to it is cast back to this type.
// - A packed field keeps this structure in its PerfettoPbPackedMsg type.
// - An incremental STRING field allocates it on the heap.
struct PerfettoPbMsgStagingWriter {
  struct PerfettoPbMsgWriter writer;
  // Null while the value fits in `buffer`. Set when the value moves to heap
  // storage.
  struct PerfettoHeapBuffer* heap_buffer;
  int32_t field_id;
  // True if this structure is on the heap. Finalization then frees it.
  bool owns_self;
  uint8_t buffer[64];
};

struct PerfettoPbMsg {
  // Pointer to a reserved, possibly unaligned varint slot of
  // PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE bytes. If not NULL,
  // PerfettoPbMsgFinalize() writes the message size into this slot.
  //
  // In proto group mode, this pointer is unused and always NULL.
  uint8_t* size_field;

  // Current size of the buffer.
  uint32_t size;

  // True after finalization. Further appends are invalid.
  //
  // PerfettoPbMsgEndNested(parent) can finalize a child again after an explicit
  // PerfettoPbMsgFinalize(child) call. This flag prevents duplicate writes,
  // including an extra proto group closing byte that corrupts the packet.
  bool is_finalized;

  // Selected by the root and inherited by its children.
  // A uint8_t fits in the existing padding beside |is_finalized|, so this field
  // does not increase the struct's size.
  uint8_t encoding;

  // In staged mode, points to the writer in PerfettoPbMsgStagingWriter.
  // Do not copy or move an active message.
  struct PerfettoPbMsgWriter* writer;

  struct PerfettoPbMsg* nested;

  // NULL for the root. Otherwise, points to the message that opened this child.
  // In proto group mode, PerfettoPbMsgFinalize() appends a closing byte only if
  // this pointer is non-NULL. The packet boundary ends the root.
  struct PerfettoPbMsg* parent;
};

// Initializes a root message with the selected encoding.
// See PerfettoPbMsgEncoding for supported operations.
static inline void PerfettoPbMsgInitWithEncoding(
    struct PerfettoPbMsg* msg,
    struct PerfettoPbMsgWriter* writer,
    enum PerfettoPbMsgEncoding encoding) {
  assert(encoding == PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED ||
         encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  msg->size_field = PERFETTO_NULL;
  msg->size = 0;
  msg->is_finalized = false;
  msg->encoding = PERFETTO_STATIC_CAST(uint8_t, encoding);
  msg->writer = writer;
  msg->nested = PERFETTO_NULL;
  msg->parent = PERFETTO_NULL;
}

// Initializes a root message with the default length-delimited encoding.
// Convenience wrapper for PerfettoPbMsgInitWithEncoding().
//
// TODO(sashwinbalaji): Migrate callers to PerfettoPbMsgInitWithEncoding() and
// remove this wrapper. Not done yet because data_source.h, track_event.h, the
// Java SDK and C SDK users outside this repo still call PerfettoPbMsgInit().
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
  // Stop when msg has no reserved length field. The root, a proto group
  // message, and an already-finalized message all lack one.
  while (msg && msg->size_field && cur_range_begin <= msg->size_field &&
         msg->size_field < cur_range_end) {
    PerfettoPbMsgPatch(msg);
    msg = msg->parent;
  }
}

// Moves a staged value from its inline buffer to heap storage, with the bytes
// written so far. Does nothing if the value is already on the heap.
//
// Call it before a length field is reserved in the value. That field must not
// move when the value grows.
static inline void PerfettoPbMsgEnsureStagingHeapBuffer(
    struct PerfettoPbMsg* msg) {
  struct PerfettoPbMsgStagingWriter* staged;
  size_t used;
  assert(!msg->is_finalized);
  assert(msg->encoding == PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED);
  staged = PERFETTO_REINTERPRET_CAST(struct PerfettoPbMsgStagingWriter*,
                                     msg->writer);
  if (staged->heap_buffer)
    return;
  used = PerfettoStreamWriterGetWrittenSize(&staged->writer.writer);
  staged->heap_buffer = PerfettoHeapBufferCreate(&staged->writer.writer);
  PerfettoStreamWriterAppendBytes(&staged->writer.writer, staged->buffer, used);
}

static inline void PerfettoPbMsgAppendBytes(struct PerfettoPbMsg* msg,
                                            const uint8_t* begin,
                                            size_t size) {
  assert(!msg->is_finalized);
  if (PERFETTO_UNLIKELY(
          size > PerfettoStreamWriterAvailableBytes(&msg->writer->writer))) {
    if (msg->encoding == PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED) {
      PerfettoPbMsgEnsureStagingHeapBuffer(msg);
    }
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

// Begins a nested message field. Use it only for fields of message type. For
// incremental STRING or PACKED fields use
// PerfettoPbMsgBeginLengthDelimitedField().
static inline void PerfettoPbMsgBeginNested(struct PerfettoPbMsg* parent,
                                            struct PerfettoPbMsg* nested,
                                            int32_t field_id) {
  // A child takes the encoding of its parent. The exception is a staged
  // value: it holds a serialized message, so its children use
  // length-delimited encoding.
  const enum PerfettoPbMsgEncoding child_encoding =
      parent->encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP
          ? PERFETTO_PB_MSG_ENCODING_PROTO_GROUP
          : PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED;
  PerfettoPbMsgInitWithEncoding(nested, parent->writer, child_encoding);
  if (parent->encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP) {
    // The closing byte written by PerfettoPbMsgFinalize() ends the child.
    PerfettoPbMsgAppendVarInt(parent, PerfettoPbMakeTagStartGroup(field_id));
  } else {
    if (parent->encoding == PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED)
      PerfettoPbMsgEnsureStagingHeapBuffer(parent);
    PerfettoPbMsgAppendVarInt(
        parent, PerfettoPbMakeTag(field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED));
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

// Begins a STRING or PACKED value on a proto group stream, in `staged`.
// PerfettoPbMsgFinalize() of `nested` writes the value to the parent. See
// PerfettoPbMsgPublishStagedPayload().
//
// - Keep `staged` valid until `nested` is finalized.
// - A bytes value can hold a serialized message. Its nested messages use
//   heap storage and length-delimited encoding.
static inline void PerfettoPbMsgBeginStaged(
    struct PerfettoPbMsg* parent,
    struct PerfettoPbMsg* nested,
    int32_t field_id,
    struct PerfettoPbMsgStagingWriter* staged) {
  staged->owns_self = false;
  staged->heap_buffer = PERFETTO_NULL;
  staged->field_id = field_id;
  staged->writer.writer.impl = PERFETTO_NULL;
  staged->writer.writer.begin = staged->buffer;
  staged->writer.writer.end = staged->buffer + sizeof(staged->buffer);
  staged->writer.writer.write_ptr = staged->buffer;
  staged->writer.writer.written_previously = 0;
  PerfettoPbMsgInit(nested, &staged->writer);
  nested->encoding = PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED;
  nested->parent = parent;
  parent->nested = nested;
}

// Begins an incremental STRING field. The length comes before the value:
//
// - Length-delimited stream: reserves space for the length, and fills it when
//   the field closes.
// - Proto group stream: stages the value on the heap. The tag, length and
//   value go to the stream when the field closes. A proto group stream cannot
//   change bytes that a reader may have read.
//
// Packed fields stage in their PerfettoPbPackedMsg type instead. See
// pb_macros.h.
static inline void PerfettoPbMsgBeginLengthDelimitedField(
    struct PerfettoPbMsg* parent,
    struct PerfettoPbMsg* nested,
    int32_t field_id) {
  if (parent->encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP) {
    struct PerfettoPbMsgStagingWriter* staged = PERFETTO_STATIC_CAST(
        struct PerfettoPbMsgStagingWriter*, malloc(sizeof(*staged)));
    // Like HeapBuffer, abort if allocation fails. The parent is unchanged.
    if (!staged)
      abort();
    PerfettoPbMsgBeginStaged(parent, nested, field_id, staged);
    staged->owns_self = true;
    return;
  }

  PerfettoPbMsgBeginNested(parent, nested, field_id);
}

static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg);

static inline void PerfettoPbMsgEndNested(struct PerfettoPbMsg* parent) {
  parent->size += PerfettoPbMsgFinalize(parent->nested);
  parent->nested = PERFETTO_NULL;
}

// Writes a staged value to the parent. PerfettoPbMsgFinalize() calls it once,
// on the first finalization of the value.
//
// 1. Write the tag and the length.
// 2. Copy the value.
// 3. Free the heap storage.
static inline void PerfettoPbMsgPublishStagedPayload(
    struct PerfettoPbMsg* msg) {
  struct PerfettoPbMsgStagingWriter* staged = PERFETTO_REINTERPRET_CAST(
      struct PerfettoPbMsgStagingWriter*, msg->writer);
  size_t size = PerfettoStreamWriterGetWrittenSize(&staged->writer.writer);
  uint8_t
      header[PERFETTO_PB_VARINT_MAX_SIZE_32 + PERFETTO_PB_VARINT_MAX_SIZE_64];
  size_t header_size;
  struct PerfettoPbMsg* parent;
  uint8_t* end = PerfettoPbWriteVarInt(
      PerfettoPbMakeTag(staged->field_id, PERFETTO_PB_WIRE_TYPE_DELIMITED),
      header);
  end = PerfettoPbWriteVarInt(size, end);
  header_size = PERFETTO_STATIC_CAST(size_t, end - header);
  parent = msg->parent;
  // Check for size overflow before writing the header.
  if (size > UINT32_MAX || header_size > UINT32_MAX - size ||
      parent->size > UINT32_MAX - size - header_size)
    abort();
  msg->size = PERFETTO_STATIC_CAST(uint32_t, size);
  // PerfettoPbMsgAppendBytes() adds the header size to the parent size.
  // PerfettoPbMsgEndNested() adds the payload size once.
  PerfettoPbMsgAppendBytes(parent, header, header_size);
  if (size > PerfettoStreamWriterAvailableBytes(&parent->writer->writer))
    PerfettoPbMsgPatchStack(parent);
  if (staged->heap_buffer) {
    PerfettoHeapBufferCopyIntoStreamWriter(
        staged->heap_buffer, &staged->writer.writer, &parent->writer->writer);
    PerfettoHeapBufferDestroy(staged->heap_buffer, &staged->writer.writer);
  } else {
    PerfettoStreamWriterAppendBytes(&parent->writer->writer, staged->buffer,
                                    size);
  }
  if (staged->owns_self)
    free(staged);
  msg->writer = PERFETTO_NULL;
}

// Finalizes this message and its children. Returns the message size.
// Repeated calls return the same size without changes.
static inline size_t PerfettoPbMsgFinalize(struct PerfettoPbMsg* msg) {
  if (msg->is_finalized)
    return msg->size;

  if (msg->nested)
    PerfettoPbMsgEndNested(msg);

  // A proto group message has no length field. Only a child needs the closing
  // byte: the packet boundary ends the root. See |parent| above.
  if (msg->encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP) {
    assert(!msg->size_field);
    if (msg->parent) {
      PerfettoPbMsgAppendByte(
          msg, PERFETTO_STATIC_CAST(uint8_t, PERFETTO_PB_PROTO_GROUP_END_BYTE));
    }
    msg->is_finalized = true;
    return msg->size;
  }

  if (msg->encoding == PERFETTO_PB_MSG_ENCODING_STAGED_LENGTH_DELIMITED)
    PerfettoPbMsgPublishStagedPayload(msg);

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

  msg->is_finalized = true;
  return msg->size;
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_MSG_H_
