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

static_assert(kMessageLengthFieldSize == sizeof(uint32_t),
              "Each length placeholder must hold a 32-bit nesting link");

constexpr uint32_t kNoOpenMessage = UINT32_MAX;

// The low three bits of a 32-bit tag hold the wire type, so a field id has at
// most 29 bits.
constexpr uint64_t kMaxFieldId = (1u << 29) - 1;

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
  // This bound comes from the trusted caller, not the packet data.
  PERFETTO_CHECK(max_output_size <= UINT32_MAX);
  output->clear();

  // Open-message stack, stored in output length placeholders:
  // - Each slot holds the output offset of its enclosing message's length
  //   field, or kNoOpenMessage if its parent is the root.
  // - Offsets survive output reallocations.
  // - A four-byte slot must fit within max_output_size <= UINT32_MAX, so its
  //   offset is at most UINT32_MAX - 4 and cannot equal kNoOpenMessage.
  uint32_t innermost_length_offset = kNoOpenMessage;
  const uint8_t* read_ptr = input_begin;

  while (read_ptr < input_end) {
    // 1. Close the innermost message and backfill its length. 0x04 is a close
    // marker only at a field boundary; field parsers consume embedded bytes.
    if (*read_ptr == kProtoGroupEndByte) {
      if (innermost_length_offset == kNoOpenMessage)
        return Reject(output, RewriteResult::kMalformedInput);
      ++read_ptr;

      const size_t length_offset = innermost_length_offset;
      PERFETTO_DCHECK(length_offset <= output->size());
      PERFETTO_DCHECK(kMessageLengthFieldSize <=
                      output->size() - length_offset);
      uint32_t enclosing_length_offset;
      memcpy(&enclosing_length_offset, output->data() + length_offset,
             sizeof(enclosing_length_offset));
      const size_t content_size =
          output->size() - length_offset - kMessageLengthFieldSize;
      if (content_size > kMaxMessageLength)
        return Reject(output, RewriteResult::kOutputTooLarge);
      WriteRedundantVarInt(static_cast<uint32_t>(content_size),
                           output->data() + length_offset);
      innermost_length_offset = enclosing_length_offset;
      continue;
    }

    // 2. Parse and validate the next field tag.
    const uint8_t* const field_begin = read_ptr;
    uint64_t tag = 0;
    // TODO(sashwinbalaji): Consider rejecting overflow bits in the tenth varint
    // byte. For now, tags, lengths and values use Protozero's ParseVarInt(),
    // which discards those bits.
    read_ptr = ParseVarInt(read_ptr, input_end, &tag);
    if (read_ptr == field_begin)
      return Reject(output, RewriteResult::kMalformedInput);

    const uint64_t field_id = GetTagFieldId(tag);
    const uint32_t wire_type = static_cast<uint32_t>(GetTagFieldType(tag));
    if (field_id == 0 || field_id > kMaxFieldId || wire_type >= 6)
      return Reject(output, RewriteResult::kMalformedInput);

    // 3. Replace a nested-message open marker with a tag and length slot.
    if (wire_type == kWireTypeStartGroup) {
      uint8_t preamble[kMaxTagEncodedSize + kMessageLengthFieldSize];
      const uint32_t length_tag =
          MakeTagLengthDelimited(static_cast<uint32_t>(field_id));
      uint8_t* const length_field = WriteVarInt(length_tag, preamble);
      memcpy(length_field, &innermost_length_offset,
             sizeof(innermost_length_offset));
      if (!AppendToOutput(preamble, length_field + kMessageLengthFieldSize,
                          output, max_output_size)) {
        return Reject(output, RewriteResult::kOutputTooLarge);
      }
      innermost_length_offset =
          static_cast<uint32_t>(output->size() - kMessageLengthFieldSize);
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
        read_ptr = ParseVarInt(read_ptr, input_end, &value);
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
        read_ptr = ParseVarInt(read_ptr, input_end, &length);
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

  if (innermost_length_offset != kNoOpenMessage)
    return Reject(output, RewriteResult::kMalformedInput);

  return RewriteResult::kSuccess;
}

}  // namespace perfetto::tracing_v2
