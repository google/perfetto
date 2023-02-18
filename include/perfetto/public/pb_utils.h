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
  PERFETTO_PB_WIRE_TYPE_DELIMITED = 2,
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

#endif  // INCLUDE_PERFETTO_PUBLIC_PB_UTILS_H_
