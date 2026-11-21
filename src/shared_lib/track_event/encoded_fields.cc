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

#include "src/shared_lib/track_event/encoded_fields.h"

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto::shlib {
namespace {

// A declared slot tells us which length-delimited payloads contain messages.
// Other payloads remain opaque strings/bytes. The depth and total input size
// bound both recursion and the optional prefix list before any session mutates.
bool ValidateMessage(const PerfettoTeHlEventFields& fields,
                     size_t begin,
                     size_t end,
                     size_t depth,
                     size_t* slot_index,
                     EncodedFieldLengths* lengths) {
  if (depth > 64)
    return false;
  const auto* data = static_cast<const uint8_t*>(fields.data);
  protozero::ProtoDecoder decoder(data + begin, end - begin);
  while (decoder.bytes_left()) {
    size_t field_begin = begin + decoder.read_offset();
    auto field = decoder.ReadField();
    if (!field.valid())
      return false;
    size_t field_end = begin + decoder.read_offset();
    if (*slot_index == fields.num_iid_slots ||
        fields.iid_slots[*slot_index].offset >= field_end) {
      continue;
    }
    size_t offset = fields.iid_slots[*slot_index].offset;
    if (offset < field_begin)
      return false;
    uint64_t tag;
    const uint8_t* value = protozero::proto_utils::ParseVarInt(
        data + field_begin, data + field_end, &tag);
    if (field.type() == protozero::proto_utils::ProtoWireType::kVarInt) {
      if (value != data + offset ||
          field_end - offset != PERFETTO_TE_HL_IID_SLOT_SIZE) {
        return false;
      }
      ++*slot_index;
      continue;
    }
    if (field.type() !=
        protozero::proto_utils::ProtoWireType::kLengthDelimited) {
      return false;
    }
    auto payload = field.as_bytes();
    size_t payload_begin = static_cast<size_t>(payload.data - data);
    size_t width = static_cast<size_t>(payload.data - value);
    if (offset < payload_begin || width == 0 ||
        width > protozero::proto_utils::kMessageLengthFieldSize) {
      return false;
    }
    size_t length_index = lengths ? lengths->size() : 0;
    if (lengths) {
      lengths->emplace_back(EncodedFieldLength{
          static_cast<size_t>(value - data), width,
          static_cast<uint32_t>(payload.size), *slot_index, 0});
    }
    if (!ValidateMessage(fields, payload_begin, field_end, depth + 1,
                         slot_index, lengths)) {
      return false;
    }
    if (lengths)
      (*lengths)[length_index].end_slot = *slot_index;
  }
  return true;
}

}  // namespace

bool ValidateEncodedFields(const PerfettoTeHlEventFields* fields,
                           size_t extra_bytes,
                           EncodedFieldLengths* lengths) {
  if (lengths)
    lengths->clear();
  size_t remaining = PERFETTO_TE_HL_MAX_ENCODED_EVENT_SIZE;
  if (extra_bytes > remaining)
    return false;
  remaining -= extra_bytes;
  if (!fields)
    return true;
  if (fields->size > remaining || (fields->size && !fields->data))
    return false;
  remaining -= fields->size;
  if (fields->num_iid_slots > fields->size / PERFETTO_TE_HL_IID_SLOT_SIZE ||
      fields->num_iid_slots > remaining / PERFETTO_TE_HL_IID_SLOT_RECORD_SIZE ||
      (fields->num_iid_slots && !fields->iid_slots)) {
    return false;
  }
  remaining -= fields->num_iid_slots * PERFETTO_TE_HL_IID_SLOT_RECORD_SIZE;

  const auto* data = static_cast<const uint8_t*>(fields->data);
  size_t end_offset = 0;
  for (size_t i = 0; i < fields->num_iid_slots; ++i) {
    const auto& slot = fields->iid_slots[i];
    if (slot.offset < end_offset || slot.offset >= fields->size ||
        fields->size - slot.offset < PERFETTO_TE_HL_IID_SLOT_SIZE ||
        slot.interned_type_id == 0 || slot.interned_type_id > 0x1FFFFFFFu ||
        slot.str_size > remaining || (slot.str_size && !slot.str)) {
      return false;
    }
    remaining -= slot.str_size;
    for (size_t j = 0; j < PERFETTO_TE_HL_IID_SLOT_SIZE - 1; ++j) {
      if (data[slot.offset + j] != 0x80)
        return false;
    }
    if (data[slot.offset + PERFETTO_TE_HL_IID_SLOT_SIZE - 1] != 0)
      return false;
    end_offset = slot.offset + PERFETTO_TE_HL_IID_SLOT_SIZE;
  }
  size_t slot_index = 0;
  return !fields->size ||
         (ValidateMessage(*fields, 0, fields->size, 0, &slot_index, lengths) &&
          slot_index == fields->num_iid_slots);
}

size_t WriteEncodedIid(uint64_t iid,
                       uint8_t destination[PERFETTO_TE_HL_IID_SLOT_SIZE]) {
  return static_cast<size_t>(
      protozero::proto_utils::WriteVarInt(iid, destination) - destination);
}

}  // namespace perfetto::shlib

bool PerfettoTeHlValidateEventFields(const PerfettoTeHlEventFields* fields) {
  return perfetto::shlib::ValidateEncodedFields(fields, 0);
}
