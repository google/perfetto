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

#include "src/tracing/v2/proto_rewriter.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto::tracing_v2 {

namespace proto_utils = ::protozero::proto_utils;

namespace {

using proto_utils::GetTagFieldId;
using proto_utils::GetTagFieldType;
using proto_utils::kMaxMessageLength;
using proto_utils::kMaxTagEncodedSize;
using proto_utils::kMessageLengthFieldSize;
using proto_utils::kProtoGroupEndByte;
using proto_utils::kWireTypeEndGroup;
using proto_utils::kWireTypeStartGroup;
using proto_utils::MakeTagLengthDelimited;
using proto_utils::ParseVarInt;
using proto_utils::ProtoWireType;
using proto_utils::WriteRedundantVarInt;
using proto_utils::WriteVarInt;

// The low three bits of a 32-bit tag hold the wire type, so a field id has at
// most 29 bits.
constexpr uint64_t kMaxFieldId = (1u << 29) - 1;

// A varint carries seven value bits per byte, so uint64_t needs at most ten.
constexpr size_t kMaxVarIntBytes = 10;

// ParseVarInt() drops overflow bits from byte ten. Reject them here while
// continuing to accept redundant encodings, as protobuf does.
const uint8_t* ParseVarIntWithinUint64(const uint8_t* start,
                                       const uint8_t* end,
                                       uint64_t* value) {
  const uint8_t* pos = ParseVarInt(start, end, value);
  if (pos == start)
    return start;
  const size_t num_bytes = static_cast<size_t>(pos - start);
  // ParseVarInt() consumes at most ten bytes. The first nine hold 63 bits, so
  // byte ten may only be 0 or 1.
  if (num_bytes == kMaxVarIntBytes && start[kMaxVarIntBytes - 1] > 1)
    return start;
  return pos;
}

bool AppendToOutput(const uint8_t* source_begin,
                    const uint8_t* source_end,
                    std::vector<uint8_t>* output,
                    size_t max_output_size) {
  const size_t size = static_cast<size_t>(source_end - source_begin);
  PERFETTO_DCHECK(output->size() <= max_output_size);
  if (size > max_output_size - output->size())
    return false;
  output->insert(output->end(), source_begin, source_end);
  return true;
}

// Never leave the caller with the prefix written before the error was found.
RewriteResult Reject(std::vector<uint8_t>* output, RewriteResult result) {
  PERFETTO_DCHECK(result != RewriteResult::kSuccess);
  output->clear();
  return result;
}

}  // namespace

RewriteResult RewriteProtoGroupToLengthDelimited(const uint8_t* input_begin,
                                                 const uint8_t* input_end,
                                                 std::vector<uint8_t>* output,
                                                 size_t max_output_size) {
  if (max_output_size > UINT32_MAX)
    return Reject(output, RewriteResult::kOutputTooLarge);
  output->clear();

  // Open-message stack, stored in output length placeholders:
  // - Each slot holds its enclosing slot's offset + 1; zero means root.
  // - Offsets survive output reallocations.
  // - A four-byte slot requires offset + 4 <= max_output_size <= UINT32_MAX,
  //   so offset + 1 cannot wrap to zero.
  uint32_t innermost_open_link = 0;
  const uint8_t* read_ptr = input_begin;

  while (read_ptr < input_end) {
    // 1. Close the innermost message and backfill its length. 0x04 is a close
    // marker only at a field boundary; field parsers consume embedded bytes.
    if (*read_ptr == kProtoGroupEndByte) {
      if (innermost_open_link == 0)
        return Reject(output, RewriteResult::kMalformedInput);
      ++read_ptr;

      const size_t length_offset = innermost_open_link - 1;
      uint32_t enclosing_link = 0;
      memcpy(&enclosing_link, output->data() + length_offset,
             sizeof(enclosing_link));
      const size_t content_size =
          output->size() - length_offset - kMessageLengthFieldSize;
      if (content_size > kMaxMessageLength)
        return Reject(output, RewriteResult::kOutputTooLarge);
      WriteRedundantVarInt(static_cast<uint32_t>(content_size),
                           output->data() + length_offset);
      innermost_open_link = enclosing_link;
      continue;
    }

    // 2. Parse and validate the next field tag.
    const uint8_t* const field_begin = read_ptr;
    uint64_t tag = 0;
    read_ptr = ParseVarIntWithinUint64(read_ptr, input_end, &tag);
    if (read_ptr == field_begin)
      return Reject(output, RewriteResult::kMalformedInput);

    const uint64_t field_id = GetTagFieldId(tag);
    const uint32_t wire_type = static_cast<uint32_t>(GetTagFieldType(tag));
    if (field_id == 0 || field_id > kMaxFieldId || wire_type >= 6)
      return Reject(output, RewriteResult::kMalformedInput);

    // 3. Replace a nested-message open marker with a tag and length slot.
    if (wire_type == kWireTypeStartGroup) {
      uint8_t tag_bytes[kMaxTagEncodedSize];
      const uint32_t length_tag =
          MakeTagLengthDelimited(static_cast<uint32_t>(field_id));
      uint8_t* const tag_end = WriteVarInt(length_tag, tag_bytes);
      if (!AppendToOutput(tag_bytes, tag_end, output, max_output_size))
        return Reject(output, RewriteResult::kOutputTooLarge);

      if (kMessageLengthFieldSize > max_output_size - output->size())
        return Reject(output, RewriteResult::kOutputTooLarge);
      const size_t length_offset = output->size();
      output->resize(length_offset + kMessageLengthFieldSize);
      memcpy(output->data() + length_offset, &innermost_open_link,
             sizeof(innermost_open_link));
      innermost_open_link = static_cast<uint32_t>(length_offset + 1);
      continue;
    }

    if (wire_type == kWireTypeEndGroup) {
      // Proto-group uses the field-id-less end byte handled above.
      return Reject(output, RewriteResult::kMalformedInput);
    }

    // 4. Validate an ordinary field, then copy it verbatim.
    switch (static_cast<ProtoWireType>(wire_type)) {
      case ProtoWireType::kVarInt: {
        const uint8_t* const value_begin = read_ptr;
        uint64_t value = 0;
        read_ptr = ParseVarIntWithinUint64(read_ptr, input_end, &value);
        if (read_ptr == value_begin)
          return Reject(output, RewriteResult::kMalformedInput);
        break;
      }
      case ProtoWireType::kFixed64:
      case ProtoWireType::kFixed32: {
        const size_t field_size =
            wire_type == static_cast<uint32_t>(ProtoWireType::kFixed64) ? 8 : 4;
        if (static_cast<size_t>(input_end - read_ptr) < field_size)
          return Reject(output, RewriteResult::kMalformedInput);
        read_ptr += field_size;
        break;
      }
      case ProtoWireType::kLengthDelimited: {
        const uint8_t* const length_begin = read_ptr;
        uint64_t length = 0;
        read_ptr = ParseVarIntWithinUint64(read_ptr, input_end, &length);
        if (read_ptr == length_begin ||
            length > static_cast<uint64_t>(input_end - read_ptr)) {
          return Reject(output, RewriteResult::kMalformedInput);
        }
        read_ptr += static_cast<size_t>(length);
        break;
      }
    }
    if (!AppendToOutput(field_begin, read_ptr, output, max_output_size))
      return Reject(output, RewriteResult::kOutputTooLarge);
  }

  if (innermost_open_link != 0)
    return Reject(output, RewriteResult::kMalformedInput);

  return RewriteResult::kSuccess;
}

}  // namespace perfetto::tracing_v2
