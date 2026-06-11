/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/protozero/proto_decoder.h"

#include <string.h>

#include <cinttypes>
#include <limits>
#include <memory>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/compiler.h"

namespace protozero {

using namespace proto_utils;

#if !PERFETTO_IS_LITTLE_ENDIAN()
#error Unimplemented for big endian archs.
#endif

const Field TypedProtoDecoderBase::kInvalidField{};

namespace {

struct ParseFieldResult {
  enum ParseResult { kAbort, kSkip, kOk };
  ParseResult parse_res;
  const uint8_t* next;
  Field field;
};

// Parses one field and returns the field itself and a pointer to the next
// field to parse. If parsing fails, the returned |next| == |buffer|.
// Force-inlined into ReadField() and the ParseAllFields() decode loop: the
// result struct is decomposed by the compiler after inlining, so it does not
// round-trip through memory.
PERFETTO_ALWAYS_INLINE ParseFieldResult
ParseOneField(const uint8_t* const buffer, const uint8_t* const end) {
  ParseFieldResult res{ParseFieldResult::kAbort, buffer, Field{}};

  // If we've already hit the end, just return an invalid field.
  if (PERFETTO_UNLIKELY(buffer >= end))
    return res;

  // Tag.
  uint64_t preamble;
  const uint8_t* pos = ParseVarIntFast(buffer, end, &preamble);
  if (PERFETTO_UNLIKELY(!pos))
    return res;  // Truncated tag.
  const uint32_t field_id =
      static_cast<uint32_t>(proto_utils::GetTagFieldId(preamble));
  if (PERFETTO_UNLIKELY(field_id == 0))
    return res;
  const uint8_t wire_type =
      static_cast<uint8_t>(proto_utils::GetTagFieldType(preamble));

  // Value. If-chain ordered by frequency: wire types within a message are
  // near-constant, so these branches predict better than a switch jump.
  uint64_t int_value = 0;
  uint32_t size = 0;
  if (PERFETTO_LIKELY(wire_type ==
                      static_cast<uint8_t>(ProtoWireType::kVarInt))) {
    const uint8_t* next = ParseVarIntFast(pos, end, &int_value);
    if (PERFETTO_UNLIKELY(!next))
      return res;
    pos = next;
  } else if (PERFETTO_LIKELY(
                 wire_type ==
                 static_cast<uint8_t>(ProtoWireType::kLengthDelimited))) {
    uint64_t payload_length;
    const uint8_t* next = ParseVarIntFast(pos, end, &payload_length);
    if (PERFETTO_UNLIKELY(!next))
      return res;
    pos = next;
    if (PERFETTO_UNLIKELY(payload_length > static_cast<uint64_t>(end - pos)))
      return res;
    int_value = reinterpret_cast<uintptr_t>(pos);
    pos += payload_length;
    if (PERFETTO_UNLIKELY(payload_length > kMaxMessageLength)) {
      // Skip the oversized field but keep parsing.
      PERFETTO_DLOG("Skipping field %" PRIu32 " because it's too big (%" PRIu64
                    " KB)",
                    field_id, payload_length / 1024);
      res.next = pos;
      res.parse_res = ParseFieldResult::kSkip;
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

  if (PERFETTO_UNLIKELY(field_id > Field::kMaxId)) {
    PERFETTO_DLOG("Skipping field %" PRIu32 " because its id > %" PRIu32,
                  field_id, Field::kMaxId);
    res.parse_res = ParseFieldResult::kSkip;
    return res;
  }

  res.parse_res = ParseFieldResult::kOk;
  res.field.initialize(field_id, wire_type, int_value, size);
  return res;
}

}  // namespace

Field ProtoDecoder::FindField(uint32_t field_id) {
  Field res{};
  auto old_position = read_ptr_;
  read_ptr_ = begin_;
  for (auto f = ReadField(); f.valid(); f = ReadField()) {
    if (f.id() == field_id) {
      res = f;
      break;
    }
  }
  read_ptr_ = old_position;
  return res;
}

Field ProtoDecoder::ReadField() {
  ParseFieldResult res;
  do {
    res = ParseOneField(read_ptr_, end_);
    read_ptr_ = res.next;
  } while (PERFETTO_UNLIKELY(res.parse_res == ParseFieldResult::kSkip));
  return res.field;
}

void TypedProtoDecoderBase::ParseAllFields() {
  // ParseOneField() is force-inlined here, so the ParseFieldResult is
  // decomposed by the compiler and each Field is written straight into its
  // storage slot.
  const uint8_t* pos = begin_;
  const uint8_t* const end = end_;
  for (;;) {
    ParseFieldResult res = ParseOneField(pos, end);
    PERFETTO_DCHECK(res.parse_res == ParseFieldResult::kAbort ||
                    res.next != pos);
    if (PERFETTO_UNLIKELY(res.parse_res == ParseFieldResult::kAbort))
      break;  // Truncated or malformed field; |pos| stays at its start.
    pos = res.next;
    if (PERFETTO_UNLIKELY(res.parse_res == ParseFieldResult::kSkip))
      continue;  // Skip the oversized field but keep parsing.

    PERFETTO_DCHECK(res.field.valid());
    const uint32_t field_id = res.field.id();

    // Fields with an id beyond the in-tree range are out-of-tree extension
    // fields; they are not stored here (callers resolve them via
    // GetExtensionSlowly(), which re-scans the buffer).
    if (PERFETTO_UNLIKELY(field_id >= num_fields_))
      continue;

    // Expand if the id is beyond the current stack extent, or the overflow
    // region is full.
    if (PERFETTO_UNLIKELY(field_id >= size_ || size_ >= capacity_))
      ExpandHeapStorage();
    PERFETTO_DCHECK(field_id < size_);

    if (PERFETTO_LIKELY(!HasField(field_id))) {
      // First time we see this field; presence is the bitmap, not validity.
      SetField(field_id);
      fields_[field_id] = res.field;
    } else {
      // Repeated field: push the previous value to the overflow region (so
      // RepeatedFieldIterator yields FIFO) and keep the last value in the slot.
      if (PERFETTO_UNLIKELY(num_fields_ > size_))
        ExpandHeapStorage();
      PERFETTO_DCHECK(size_ < capacity_);
      fields_[size_++] = fields_[field_id];
      fields_[field_id] = res.field;
    }
  }
  read_ptr_ = pos;
}

void TypedProtoDecoderBase::ExpandHeapStorage() {
  // When we expand the heap we must ensure that we have at very last capacity
  // to deal with all known fields plus at least one repeated field. We go +2048
  // here based on observations on a large 4GB android trace. This is to avoid
  // trivial re-allocations when dealing with repeated fields of a message that
  // has > INITIAL_STACK_CAPACITY fields.
  const uint32_t min_capacity = num_fields_ + 2048;  // Any num >= +1 will do.
  const uint32_t new_capacity = std::max(capacity_ * 2, min_capacity);
  PERFETTO_CHECK(new_capacity > size_ && new_capacity > num_fields_);
  std::unique_ptr<Field[]> new_storage(new Field[new_capacity]);

  static_assert(std::is_trivially_constructible<Field>::value,
                "Field must be trivially constructible");
  static_assert(std::is_trivially_copyable<Field>::value,
                "Field must be trivially copyable");

  // Newly-exposed known-field slots are left uninitialized; reads are gated on
  // the presence bitmap. The repeated slots are written linearly with no gaps,
  // always before incrementing |size_|.
  const uint32_t new_size = std::max(size_, num_fields_);
  memcpy(&new_storage[0], fields_, sizeof(Field) * size_);

  heap_storage_ = std::move(new_storage);
  fields_ = &heap_storage_[0];
  capacity_ = new_capacity;
  size_ = new_size;
}

}  // namespace protozero
