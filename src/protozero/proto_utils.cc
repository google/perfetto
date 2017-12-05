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

#include "perfetto/protozero/proto_utils.h"

#include <string.h>

#include <limits>

#include "perfetto/base/logging.h"

#define PERFETTO_CHECK_PTR_LE(a, b)                \
  PERFETTO_CHECK(reinterpret_cast<uintptr_t>(a) <= \
                 reinterpret_cast<uintptr_t>(b))

namespace protozero {
namespace proto_utils {

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BYTE_SWAP_TO_LE32(x) (x)
#define BYTE_SWAP_TO_LE64(x) (x)
#else
#error Unimplemented for big endian archs.
#endif

const uint8_t* ParseVarInt(const uint8_t* start,
                           const uint8_t* end,
                           uint64_t* value) {
  const uint8_t* pos = start;
  uint64_t shift = 0;
  *value = 0;
  do {
    PERFETTO_CHECK_PTR_LE(pos, end - 1);
    PERFETTO_DCHECK(shift < 64ull);
    *value |= static_cast<uint64_t>(*pos & 0x7f) << shift;
    shift += 7;
  } while (*pos++ & 0x80);
  return pos;
}

const uint8_t* ParseField(const uint8_t* start,
                          const uint8_t* end,
                          uint32_t* field_id,
                          FieldType* field_type,
                          uint64_t* field_intvalue) {
  // The first byte of a proto field is structured as follows:
  // The least 3 significant bits determine the field type.
  // The most 5 significant bits determine the field id. If MSB == 1, the
  // field id continues on the next bytes following the VarInt encoding.
  const uint8_t kFieldTypeNumBits = 3;
  const uint8_t kFieldTypeMask = (1 << kFieldTypeNumBits) - 1;  // 0000 0111;

  const uint8_t* pos = start;
  PERFETTO_CHECK_PTR_LE(pos, end - 1);
  *field_type = static_cast<FieldType>(*pos & kFieldTypeMask);

  uint64_t raw_field_id;
  pos = ParseVarInt(pos, end, &raw_field_id);
  raw_field_id >>= kFieldTypeNumBits;

  PERFETTO_DCHECK(raw_field_id <= std::numeric_limits<uint32_t>::max());
  *field_id = static_cast<uint32_t>(raw_field_id);

  switch (*field_type) {
    case kFieldTypeFixed64: {
      PERFETTO_CHECK_PTR_LE(pos + sizeof(uint64_t), end);
      memcpy(field_intvalue, pos, sizeof(uint64_t));
      *field_intvalue = BYTE_SWAP_TO_LE64(*field_intvalue);
      pos += sizeof(uint64_t);
      break;
    }
    case kFieldTypeFixed32: {
      PERFETTO_CHECK_PTR_LE(pos + sizeof(uint32_t), end);
      uint32_t tmp;
      memcpy(&tmp, pos, sizeof(uint32_t));
      *field_intvalue = BYTE_SWAP_TO_LE32(tmp);
      pos += sizeof(uint32_t);
      break;
    }
    case kFieldTypeVarInt: {
      pos = ParseVarInt(pos, end, field_intvalue);
      break;
    }
    case kFieldTypeLengthDelimited: {
      pos = ParseVarInt(pos, end, field_intvalue);
      pos += *field_intvalue;
      PERFETTO_CHECK_PTR_LE(pos, end);
      break;
    }
  }
  return pos;
}

}  // namespace proto_utils
}  // namespace protozero
