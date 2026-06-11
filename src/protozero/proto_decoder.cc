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
ParseFieldResult ParseOneField(const uint8_t* const buffer,
                               const uint8_t* const end) {
  ParseFieldResult res{ParseFieldResult::kAbort, buffer, Field{}};

  const uint8_t* pos = buffer;

  // If we've already hit the end, just return an invalid field.
  if (PERFETTO_UNLIKELY(pos >= end))
    return res;

  uint64_t preamble = 0;
  if (PERFETTO_LIKELY(*pos < 0x80)) {  // Fastpath for fields with ID < 16.
    preamble = *(pos++);
  } else {
    const uint8_t* next = ParseVarInt(pos, end, &preamble);
    if (PERFETTO_UNLIKELY(pos == next))
      return res;
    pos = next;
  }

  uint32_t field_id =
      static_cast<uint32_t>(proto_utils::GetTagFieldId(preamble));
  if (field_id == 0 || pos >= end)
    return res;

  auto field_type =
      static_cast<uint8_t>(proto_utils::GetTagFieldType(preamble));
  const uint8_t* new_pos = pos;
  uint64_t int_value = 0;
  uint64_t size = 0;

  // If-chain ordered by frequency: see ParseAllFields().
  if (PERFETTO_LIKELY(field_type ==
                      static_cast<uint8_t>(ProtoWireType::kVarInt))) {
    new_pos = ParseVarInt(pos, end, &int_value);

    // new_pos not being greater than pos means ParseVarInt could not fully
    // parse the number. This is because we are out of space in the buffer.
    // Set the id to zero and return but don't update the offset so a future
    // read can read this field.
    if (PERFETTO_UNLIKELY(new_pos == pos))
      return res;
  } else if (PERFETTO_LIKELY(
                 field_type ==
                 static_cast<uint8_t>(ProtoWireType::kLengthDelimited))) {
    uint64_t payload_length;
    new_pos = ParseVarInt(pos, end, &payload_length);
    if (PERFETTO_UNLIKELY(new_pos == pos))
      return res;

    // ParseVarInt guarantees that |new_pos| <= |end| when it succeeds;
    if (payload_length > static_cast<uint64_t>(end - new_pos))
      return res;

    const uintptr_t payload_start = reinterpret_cast<uintptr_t>(new_pos);
    int_value = payload_start;
    size = payload_length;
    new_pos += payload_length;
  } else if (field_type == static_cast<uint8_t>(ProtoWireType::kFixed64)) {
    new_pos = pos + sizeof(uint64_t);
    if (PERFETTO_UNLIKELY(new_pos > end))
      return res;
    memcpy(&int_value, pos, sizeof(uint64_t));
  } else if (field_type == static_cast<uint8_t>(ProtoWireType::kFixed32)) {
    new_pos = pos + sizeof(uint32_t);
    if (PERFETTO_UNLIKELY(new_pos > end))
      return res;
    memcpy(&int_value, pos, sizeof(uint32_t));
  } else {
    PERFETTO_DLOG("Invalid proto field type: %u", field_type);
    return res;
  }

  res.next = new_pos;

  if (PERFETTO_UNLIKELY(field_id > Field::kMaxId)) {
    PERFETTO_DLOG("Skipping field %" PRIu32 " because its id > %" PRIu32,
                  field_id, Field::kMaxId);
    res.parse_res = ParseFieldResult::kSkip;
    return res;
  }

  if (PERFETTO_UNLIKELY(size > proto_utils::kMaxMessageLength)) {
    PERFETTO_DLOG("Skipping field %" PRIu32 " because it's too big (%" PRIu64
                  " KB)",
                  field_id, size / 1024);
    res.parse_res = ParseFieldResult::kSkip;
    return res;
  }

  res.parse_res = ParseFieldResult::kOk;
  res.field.initialize(field_id, field_type, int_value,
                       static_cast<uint32_t>(size));
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
  // Inlined decode loop: unlike ReadField()/ParseOneField(), this writes each
  // Field straight into its storage slot, avoiding the per-field
  // ParseFieldResult round-trip.
  const uint8_t* pos = begin_;
  const uint8_t* const end = end_;
  while (pos < end) {
    const uint8_t* const field_start = pos;

    // Tag.
    uint64_t preamble;
    const uint8_t* tag_next = ParseVarIntFast(pos, end, &preamble);
    if (PERFETTO_UNLIKELY(!tag_next))
      break;  // Truncated tag.
    pos = tag_next;
    const uint32_t field_id = static_cast<uint32_t>(preamble >> 3);
    if (PERFETTO_UNLIKELY(field_id == 0)) {
      pos = field_start;
      break;
    }
    const uint8_t wire_type = static_cast<uint8_t>(preamble & 7u);

    // Value. If-chain ordered by frequency: wire types within a message are
    // near-constant, so these branches predict better than a switch jump.
    uint64_t int_value = 0;
    uint32_t size = 0;
    if (PERFETTO_LIKELY(wire_type ==
                        static_cast<uint8_t>(ProtoWireType::kVarInt))) {
      const uint8_t* next = ParseVarIntFast(pos, end, &int_value);
      if (PERFETTO_UNLIKELY(!next)) {
        pos = field_start;
        break;
      }
      pos = next;
    } else if (PERFETTO_LIKELY(
                   wire_type ==
                   static_cast<uint8_t>(ProtoWireType::kLengthDelimited))) {
      uint64_t payload_length;
      const uint8_t* next = ParseVarIntFast(pos, end, &payload_length);
      if (PERFETTO_UNLIKELY(!next)) {
        pos = field_start;
        break;
      }
      pos = next;
      if (PERFETTO_UNLIKELY(payload_length >
                            static_cast<uint64_t>(end - pos))) {
        pos = field_start;
        break;
      }
      int_value = reinterpret_cast<uintptr_t>(pos);
      pos += payload_length;
      if (PERFETTO_UNLIKELY(payload_length > kMaxMessageLength))
        continue;  // Skip the oversized field but keep parsing.
      size = static_cast<uint32_t>(payload_length);
    } else if (wire_type == static_cast<uint8_t>(ProtoWireType::kFixed64)) {
      if (PERFETTO_UNLIKELY(end - pos <
                            static_cast<ptrdiff_t>(sizeof(uint64_t)))) {
        pos = field_start;
        break;
      }
      memcpy(&int_value, pos, sizeof(uint64_t));
      pos += sizeof(uint64_t);
    } else if (wire_type == static_cast<uint8_t>(ProtoWireType::kFixed32)) {
      if (PERFETTO_UNLIKELY(end - pos <
                            static_cast<ptrdiff_t>(sizeof(uint32_t)))) {
        pos = field_start;
        break;
      }
      uint32_t v32;
      memcpy(&v32, pos, sizeof(uint32_t));
      int_value = v32;
      pos += sizeof(uint32_t);
    } else {
      pos = field_start;
      break;  // Invalid wire type.
    }

    // Fields with an id beyond the in-tree range are out-of-tree extension
    // fields; they are not stored here (callers resolve them via
    // GetExtensionSlowly(), which re-scans the buffer).
    if (PERFETTO_UNLIKELY(field_id >= num_fields_))
      continue;

    // Expand if the id is beyond the current stack extent, or the overflow
    // region is full.
    if (PERFETTO_UNLIKELY(field_id >= size_ || size_ >= capacity_))
      ExpandHeapStorage();

    if (PERFETTO_LIKELY(!HasField(field_id))) {
      // First time we see this field; presence is the bitmap, not validity.
      SetField(field_id);
      fields_[field_id].initialize(field_id, wire_type, int_value, size);
    } else {
      // Repeated field: push the previous value to the overflow region (so
      // RepeatedFieldIterator yields FIFO) and keep the last value in the slot.
      if (PERFETTO_UNLIKELY(num_fields_ > size_))
        ExpandHeapStorage();
      fields_[size_++] = fields_[field_id];
      fields_[field_id].initialize(field_id, wire_type, int_value, size);
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
