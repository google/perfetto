/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PROTO_UTILS_H_
#define INCLUDE_PERFETTO_PROTOZERO_PROTO_UTILS_H_

#include <inttypes.h>
#include <stddef.h>

#include <type_traits>

namespace protozero {
namespace proto_utils {

// See https://developers.google.com/protocol-buffers/docs/encoding wire types.

enum FieldType : uint32_t {
  kFieldTypeVarInt = 0,
  kFieldTypeFixed64 = 1,
  kFieldTypeLengthDelimited = 2,
  kFieldTypeFixed32 = 5,
};

// Maximum message size supported: 256 MiB (4 x 7-bit due to varint encoding).
constexpr size_t kMessageLengthFieldSize = 4;
constexpr size_t kMaxMessageLength = (1u << (kMessageLengthFieldSize * 7)) - 1;

// Field tag is encoded as 32-bit varint (5 bytes at most).
// Largest value of simple (not length-delimited) field is 64-bit varint
// (10 bytes at most). 15 bytes buffer is enough to store a simple field.
constexpr size_t kMaxTagEncodedSize = 5;
constexpr size_t kMaxSimpleFieldEncodedSize = kMaxTagEncodedSize + 10;

// Proto types: (int|uint|sint)(32|64), bool, enum.
constexpr uint32_t MakeTagVarInt(uint32_t field_id) {
  return (field_id << 3) | kFieldTypeVarInt;
}

// Proto types: fixed64, sfixed64, fixed32, sfixed32, double, float.
template <typename T>
constexpr uint32_t MakeTagFixed(uint32_t field_id) {
  static_assert(sizeof(T) == 8 || sizeof(T) == 4, "Value must be 4 or 8 bytes");
  return (field_id << 3) |
         (sizeof(T) == 8 ? kFieldTypeFixed64 : kFieldTypeFixed32);
}

// Proto types: string, bytes, embedded messages.
constexpr uint32_t MakeTagLengthDelimited(uint32_t field_id) {
  return (field_id << 3) | kFieldTypeLengthDelimited;
}

// Proto types: sint64, sint32.
template <typename T>
inline typename std::make_unsigned<T>::type ZigZagEncode(T value) {
  return static_cast<typename std::make_unsigned<T>::type>(
      (value << 1) ^ (value >> (sizeof(T) * 8 - 1)));
}

template <typename T>
inline uint8_t* WriteVarInt(T value, uint8_t* target) {
  // Avoid arithmetic (sign expanding) shifts.
  using UnsignedType = typename std::make_unsigned<T>::type;
  UnsignedType unsigned_value = static_cast<UnsignedType>(value);

  while (unsigned_value >= 0x80) {
    *target++ = static_cast<uint8_t>(unsigned_value) | 0x80;
    unsigned_value >>= 7;
  }
  *target = static_cast<uint8_t>(unsigned_value);
  return target + 1;
}

// Writes a fixed-size redundant encoding of the given |value|. This is
// used to backfill fixed-size reservations for the length field using a
// non-canonical varint encoding (e.g. \x81\x80\x80\x00 instead of \x01).
// See https://github.com/google/protobuf/issues/1530.
// In particular, this is used for nested messages. The size of a nested message
// is not known until all its field have been written. |kMessageLengthFieldSize|
// bytes are reserved to encode the size field and backfilled at the end.
inline void WriteRedundantVarInt(uint32_t value, uint8_t* buf) {
  for (size_t i = 0; i < kMessageLengthFieldSize; ++i) {
    const uint8_t msb = (i < kMessageLengthFieldSize - 1) ? 0x80 : 0;
    buf[i] = static_cast<uint8_t>(value) | msb;
    value >>= 7;
  }
}

template <uint32_t field_id>
void StaticAssertSingleBytePreamble() {
  static_assert(field_id < 16,
                "Proto field id too big to fit in a single byte preamble");
};

// Parses a VarInt from the encoded buffer [start, end). |end| is STL-style and
// points one byte past the end of buffer.
// The parsed int value is stored in the output arg |value|. Returns a pointer
// to the next unconsumed byte (so start < retval <= end) or |start| if the
// VarInt could not be fully parsed because there was not enough space in the
// buffer.
const uint8_t* ParseVarInt(const uint8_t* start,
                           const uint8_t* end,
                           uint64_t* value);

}  // namespace proto_utils
}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTO_UTILS_H_
