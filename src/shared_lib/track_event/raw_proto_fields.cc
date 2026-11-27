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

#include "src/shared_lib/track_event/raw_proto_fields.h"

#include <stdint.h>

#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/abi/track_event_hl_abi.h"
#include "perfetto/public/compiler.h"

namespace perfetto::shlib {
namespace {

using protozero::proto_utils::ParseVarInt;
using protozero::proto_utils::ProtoWireType;

// Advances past one field value at `pos` (bounded by `buf_end`), or returns
// nullptr if the wire type is invalid or the value is truncated.
const uint8_t* SkipValue(uint32_t wire_type,
                         const uint8_t* pos,
                         const uint8_t* buf_end) {
  // Switch on raw 3-bit wire type so groups (3, 4) and invalid values (6, 7)
  // hit default.
  switch (wire_type) {
    case static_cast<uint32_t>(ProtoWireType::kVarInt): {
      uint64_t unused = 0;
      const uint8_t* next = ParseVarInt(pos, buf_end, &unused);
      return next == pos ? nullptr : next;
    }
    case static_cast<uint32_t>(ProtoWireType::kFixed64):
      return buf_end - pos >= 8 ? pos + 8 : nullptr;
    case static_cast<uint32_t>(ProtoWireType::kFixed32):
      return buf_end - pos >= 4 ? pos + 4 : nullptr;
    case static_cast<uint32_t>(ProtoWireType::kLengthDelimited): {
      uint64_t payload_size = 0;
      const uint8_t* payload = ParseVarInt(pos, buf_end, &payload_size);
      if (PERFETTO_UNLIKELY(payload == pos)) {
        return nullptr;
      }
      if (PERFETTO_UNLIKELY(payload_size >
                            static_cast<uint64_t>(buf_end - payload))) {
        return nullptr;
      }
      return payload + payload_size;
    }
    default:  // Groups and invalid wire types.
      return nullptr;
  }
}

}  // namespace

bool ValidateRawProtoFields(const void* data, size_t size, size_t extra_bytes) {
  constexpr size_t kMaxSize = PERFETTO_TE_HL_MAX_RAW_PROTO_EVENT_SIZE;
  // The largest valid protobuf field number.
  constexpr uint32_t kMaxFieldId = (1u << 29) - 1;

  // Fields plus extra_bytes (event name) must fit in kMaxSize.
  if (PERFETTO_UNLIKELY(extra_bytes > kMaxSize ||
                        size > kMaxSize - extra_bytes)) {
    return false;
  }
  if (PERFETTO_UNLIKELY(size && !data)) {
    return false;
  }

  const auto* pos = static_cast<const uint8_t*>(data);
  const uint8_t* const end = pos + size;
  while (pos < end) {
    uint64_t tag = 0;
    const uint8_t* value = ParseVarInt(pos, end, &tag);
    if (PERFETTO_UNLIKELY(value == pos || tag > UINT32_MAX)) {
      return false;
    }
    const uint32_t field_id = static_cast<uint32_t>(tag >> 3);
    if (PERFETTO_UNLIKELY(field_id == 0 || field_id > kMaxFieldId)) {
      return false;
    }

    pos = SkipValue(static_cast<uint32_t>(tag & 7), value, end);
    if (PERFETTO_UNLIKELY(!pos)) {
      return false;
    }
  }
  return true;
}

}  // namespace perfetto::shlib
