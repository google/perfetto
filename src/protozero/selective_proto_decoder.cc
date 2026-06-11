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

#include "perfetto/protozero/selective_proto_decoder.h"

#include <string.h>

#include "perfetto/base/compiler.h"
#include "src/protozero/decode_field_inl.h"

namespace protozero {

#if !PERFETTO_IS_LITTLE_ENDIAN()
#error Unimplemented for big endian archs.
#endif

void SelectiveProtoDecoderBase::ParseAllFields() {
  // Same decode loop as TypedProtoDecoderBase::ParseAllFields(), but fields
  // are routed by the explicit-set bitmap: explicit fields go to the dense
  // storage (last occurrence wins), everything else is appended to the
  // unknown array (if enabled).
  const uint8_t* pos = begin_;
  const uint8_t* const end = end_;
  while (pos < end) {
    internal::DecodedField f = internal::DecodeOneField(pos, end);
    if (PERFETTO_UNLIKELY(f.status == internal::DecodedField::Status::kAbort))
      break;  // Truncated or malformed field; |pos| stays at its start.
    pos = f.next;
    if (PERFETTO_UNLIKELY(f.status == internal::DecodedField::Status::kSkip))
      continue;  // Skip the oversized field but keep parsing.

    const uint32_t field_id = f.field_id;
    if (PERFETTO_LIKELY(IsExplicit(field_id))) {
      // Singular semantics: last occurrence wins.
      SetField(field_id);
      fields_[field_id].initialize(field_id, f.wire_type, f.int_value, f.size);
    } else if (unknown_capacity_ != 0) {
      // Field::id_ is 24 bits; ids beyond that cannot be represented.
      if (PERFETTO_UNLIKELY(field_id > Field::kMaxId))
        continue;
      if (PERFETTO_UNLIKELY(unknown_size_ == unknown_capacity_))
        ExpandUnknownStorage();
      unknown_[unknown_size_++].initialize(field_id, f.wire_type, f.int_value,
                                           f.size);
    }
  }
  read_ptr_ = pos;
}

void SelectiveProtoDecoderBase::ExpandUnknownStorage() {
  const uint32_t new_capacity = unknown_capacity_ * 2;
  std::unique_ptr<Field[]> new_storage(new Field[new_capacity]);
  memcpy(&new_storage[0], unknown_, sizeof(Field) * unknown_size_);
  heap_unknown_ = std::move(new_storage);
  unknown_ = heap_unknown_.get();
  unknown_capacity_ = new_capacity;
}

}  // namespace protozero
