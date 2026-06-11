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

#ifndef SRC_PROTOZERO_DECODE_FIELD_INL_H_
#define SRC_PROTOZERO_DECODE_FIELD_INL_H_

#include <stdint.h>
#include <string.h>

#include <cinttypes>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/compiler.h"

namespace protozero {
namespace internal {

// Result of decoding a single field (tag + value) from the wire.
struct DecodedField {
  enum class Status : uint8_t {
    kOk,
    // The field was walked past but should not be stored (oversized payload).
    kSkip,
    // Truncated or malformed input: stop parsing, |next| == the field start.
    kAbort,
  };

  const uint8_t* next;
  uint64_t int_value;
  uint32_t field_id;
  uint32_t size;
  uint8_t wire_type;
  Status status;
};

// Decodes the field starting at |pos| (which must be < |end|). Force-inlined
// into the decode loop of TypedProtoDecoderBase and into ParseOneField(): the
// result struct is decomposed by the compiler after inlining, so it does not
// round-trip through memory.
PERFETTO_ALWAYS_INLINE inline DecodedField DecodeOneField(
    const uint8_t* const field_start,
    const uint8_t* const end) {
  using proto_utils::ProtoWireType;
  DecodedField res;
  res.status = DecodedField::Status::kAbort;
  res.next = field_start;
  res.int_value = 0;
  res.field_id = 0;
  res.size = 0;
  res.wire_type = 0;

  // Tag.
  uint64_t preamble;
  const uint8_t* pos =
      proto_utils::ParseVarIntFast(field_start, end, &preamble);
  if (PERFETTO_UNLIKELY(!pos))
    return res;  // Truncated tag.
  const uint32_t field_id = static_cast<uint32_t>(preamble >> 3);
  if (PERFETTO_UNLIKELY(field_id == 0))
    return res;
  const uint8_t wire_type = static_cast<uint8_t>(preamble & 7u);

  // Value. If-chain ordered by frequency: wire types within a message are
  // near-constant, so these branches predict better than a switch jump.
  uint64_t int_value = 0;
  uint32_t size = 0;
  if (PERFETTO_LIKELY(wire_type ==
                      static_cast<uint8_t>(ProtoWireType::kVarInt))) {
    const uint8_t* next = proto_utils::ParseVarIntFast(pos, end, &int_value);
    if (PERFETTO_UNLIKELY(!next))
      return res;
    pos = next;
  } else if (PERFETTO_LIKELY(
                 wire_type ==
                 static_cast<uint8_t>(ProtoWireType::kLengthDelimited))) {
    uint64_t payload_length;
    const uint8_t* next =
        proto_utils::ParseVarIntFast(pos, end, &payload_length);
    if (PERFETTO_UNLIKELY(!next))
      return res;
    pos = next;
    if (PERFETTO_UNLIKELY(payload_length > static_cast<uint64_t>(end - pos)))
      return res;
    int_value = reinterpret_cast<uintptr_t>(pos);
    pos += payload_length;
    if (PERFETTO_UNLIKELY(payload_length > proto_utils::kMaxMessageLength)) {
      // Skip the oversized field but keep parsing.
      PERFETTO_DLOG("Skipping field %" PRIu32 " because it's too big (%" PRIu64
                    " KB)",
                    field_id, payload_length / 1024);
      res.next = pos;
      res.field_id = field_id;
      res.wire_type = wire_type;
      res.status = DecodedField::Status::kSkip;
      return res;
    }
    size = static_cast<uint32_t>(payload_length);
  } else if (wire_type == static_cast<uint8_t>(ProtoWireType::kFixed64)) {
    if (PERFETTO_UNLIKELY(end - pos < static_cast<ptrdiff_t>(sizeof(uint64_t))))
      return res;
    memcpy(&int_value, pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);
  } else if (wire_type == static_cast<uint8_t>(ProtoWireType::kFixed32)) {
    if (PERFETTO_UNLIKELY(end - pos < static_cast<ptrdiff_t>(sizeof(uint32_t))))
      return res;
    uint32_t v32;
    memcpy(&v32, pos, sizeof(uint32_t));
    int_value = v32;
    pos += sizeof(uint32_t);
  } else {
    PERFETTO_DLOG("Invalid proto field type: %u", wire_type);
    return res;
  }

  res.next = pos;
  res.int_value = int_value;
  res.field_id = field_id;
  res.size = size;
  res.wire_type = wire_type;
  res.status = DecodedField::Status::kOk;
  return res;
}

}  // namespace internal
}  // namespace protozero

#endif  // SRC_PROTOZERO_DECODE_FIELD_INL_H_
