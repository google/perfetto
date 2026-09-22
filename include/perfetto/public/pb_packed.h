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
#include <string.h>

#include "perfetto/public/compiler.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/pb_utils.h"

// Use this API to write packed repeated fields as nested messages.
// The generated begin and end functions accept the PerfettoPbPackedMsg type
// that matches the field type.
//
// Storage depends on the stream format:
// - Length-delimited streams write values directly to the output. They reserve
//   four bytes for the field length. This can add overhead for small fields.
// - Proto-group streams store up to 64 bytes in an inline buffer. They use heap
//   storage for larger payloads. The end function writes the length and payload
//   to the output.

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
// Implementations of struct PerfettoPbPackedMsg for all supported field types.
// ***
struct PerfettoPbPackedMsgUint64 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgUint64Append(
    struct PerfettoPbPackedMsgUint64* buf,
    uint64_t value) {
  PerfettoPbMsgAppendVarInt(&buf->msg, value);
}

struct PerfettoPbPackedMsgUint32 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgUint32Append(
    struct PerfettoPbPackedMsgUint32* buf,
    uint32_t value) {
  PerfettoPbMsgAppendVarInt(&buf->msg, value);
}

struct PerfettoPbPackedMsgInt64 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgInt64Append(
    struct PerfettoPbPackedMsgInt64* buf,
    int64_t value) {
  PerfettoPbMsgAppendVarInt(&buf->msg, PERFETTO_STATIC_CAST(uint64_t, value));
}

struct PerfettoPbPackedMsgInt32 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgInt32Append(
    struct PerfettoPbPackedMsgInt32* buf,
    int32_t value) {
  PerfettoPbMsgAppendVarInt(&buf->msg, PERFETTO_STATIC_CAST(uint64_t, value));
}

struct PerfettoPbPackedMsgSint64 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgSint64Append(
    struct PerfettoPbPackedMsgSint64* buf,
    int64_t value) {
  uint64_t encoded = PerfettoPbZigZagEncode64(value);
  PerfettoPbMsgAppendVarInt(&buf->msg, encoded);
}

struct PerfettoPbPackedMsgSint32 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgSint32Append(
    struct PerfettoPbPackedMsgSint32* buf,
    int32_t value) {
  uint64_t encoded =
      PerfettoPbZigZagEncode64(PERFETTO_STATIC_CAST(int64_t, value));
  PerfettoPbMsgAppendVarInt(&buf->msg, encoded);
}

struct PerfettoPbPackedMsgFixed64 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgFixed64Append(
    struct PerfettoPbPackedMsgFixed64* buf,
    uint64_t value) {
  PerfettoPbMsgAppendFixed64(&buf->msg, value);
}

struct PerfettoPbPackedMsgFixed32 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgFixed32Append(
    struct PerfettoPbPackedMsgFixed32* buf,
    uint32_t value) {
  PerfettoPbMsgAppendFixed32(&buf->msg, value);
}

struct PerfettoPbPackedMsgSfixed64 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgSfixed64Append(
    struct PerfettoPbPackedMsgSfixed64* buf,
    int64_t value) {
  uint64_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbMsgAppendFixed64(&buf->msg, encoded);
}

struct PerfettoPbPackedMsgSfixed32 {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgSfixed32Append(
    struct PerfettoPbPackedMsgSfixed32* buf,
    int32_t value) {
  uint32_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbMsgAppendFixed32(&buf->msg, encoded);
}

struct PerfettoPbPackedMsgDouble {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgDoubleAppend(
    struct PerfettoPbPackedMsgDouble* buf,
    double value) {
  uint64_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbMsgAppendFixed64(&buf->msg, encoded);
}

struct PerfettoPbPackedMsgFloat {
  struct PerfettoPbMsg msg;
  struct PerfettoPbMsgStagingWriter staged;
};
static inline void PerfettoPbPackedMsgFloatAppend(
    struct PerfettoPbPackedMsgFloat* buf,
    float value) {
  uint32_t encoded;
  memcpy(&encoded, &value, sizeof(encoded));
  PerfettoPbMsgAppendFixed32(&buf->msg, encoded);
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_PACKED_H_
