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

#ifndef INCLUDE_PERFETTO_PUBLIC_PB_UTILS_H_
#define INCLUDE_PERFETTO_PUBLIC_PB_UTILS_H_

#include <stdint.h>

#include "perfetto/public/compiler.h"

// Type of fields that can be found in a protobuf serialized message.
enum PerfettoPbWireType {
  PERFETTO_PB_WIRE_TYPE_VARINT = 0,
  PERFETTO_PB_WIRE_TYPE_FIXED64 = 1,
  PERFETTO_PB_WIRE_TYPE_DELIMITED = 2,
  PERFETTO_PB_WIRE_TYPE_FIXED32 = 5,
};

// Creates a field tag, which encodes the field type and the field id.
static inline uint32_t PerfettoPbMakeTag(int32_t field_id,
                                         enum PerfettoPbWireType wire_type) {
  return ((PERFETTO_STATIC_CAST(uint32_t, field_id)) << 3) |
         PERFETTO_STATIC_CAST(uint32_t, wire_type);
}

enum {
  // Maximum bytes size of a 64-bit integer encoded as a VarInt.
  PERFETTO_PB_VARINT_MAX_SIZE_64 = 10,
  // Maximum bytes size of a 32-bit integer encoded as a VarInt.
  PERFETTO_PB_VARINT_MAX_SIZE_32 = 5,
};

// Encodes `value` as a VarInt into `*dst`.
//
// `dst` must point into a buffer big enough to represent `value`:
// PERFETTO_PB_VARINT_MAX_SIZE_* can help.
static inline uint8_t* PerfettoPbWriteVarInt(uint64_t value, uint8_t* dst) {
  uint8_t byte;
  while (value >= 0x80) {
    byte = (value & 0x7f) | 0x80;
    *dst++ = byte;
    value >>= 7;
  }
  byte = value & 0x7f;
  *dst++ = byte;

  return dst;
}

// Parses a VarInt from the encoded buffer [start, end). |end| is STL-style and
// points one byte past the end of buffer.
// The parsed int value is stored in the output arg |value|. Returns a pointer
// to the next unconsumed byte (so start < retval <= end) or |start| if the
// VarInt could not be fully parsed because there was not enough space in the
// buffer.
static inline const uint8_t* PerfettoPbParseVarInt(const uint8_t* start,
                                                   const uint8_t* end,
                                                   uint64_t* out_value) {
  const uint8_t* pos = start;
  uint64_t value = 0;
  for (uint32_t shift = 0; pos < end && shift < 64u; shift += 7) {
    // Cache *pos into |cur_byte| to prevent that the compiler dereferences the
    // pointer twice (here and in the if() below) due to char* aliasing rules.
    uint8_t cur_byte = *pos++;
    value |= PERFETTO_STATIC_CAST(uint64_t, cur_byte & 0x7f) << shift;
    if ((cur_byte & 0x80) == 0) {
      // In valid cases we get here.
      *out_value = value;
      return pos;
    }
  }
  *out_value = 0;
  return start;
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_UTILS_H_
