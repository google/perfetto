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
namespace {

// Protobuf field ids are 29 bits.
constexpr uint64_t kMaxFieldId = (1u << 29) - 1;

constexpr size_t kMaxVarIntBytes = 10;
// A varint byte uses its low seven bits for data and its high bit to signal
// that another byte follows.
constexpr uint8_t kVarIntPayloadMask = (1u << 7) - 1;

// ParseVarInt() drops overflow bits from byte ten. Reject them here while
// continuing to accept redundant encodings, as protobuf does.
const uint8_t* ParseVarIntWithinUint64(const uint8_t* start,
                                       const uint8_t* end,
                                       uint64_t* value) {
  const uint8_t* pos = protozero::proto_utils::ParseVarInt(start, end, value);
  if (pos == start)
    return start;
  const size_t num_bytes = static_cast<size_t>(pos - start);
  // The first nine bytes hold 63 bits, so byte ten may only contain 0 or 1.
  if (num_bytes == kMaxVarIntBytes &&
      (start[kMaxVarIntBytes - 1] & kVarIntPayloadMask) > 1)
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
  using protozero::proto_utils::kMaxMessageLength;
  using protozero::proto_utils::kMessageLengthFieldSize;
  using protozero::proto_utils::ProtoWireType;

  output->clear();

  // Each open nested message owns a four-byte length placeholder in the
  // output. Until it closes, the placeholder holds the offset of the enclosing
  // message's placeholder plus one (zero means the root), so the open messages
  // form a linked list through the output and only the innermost is kept
  // here. Offsets rather than pointers, because the vector reallocates.
  uint32_t innermost_open_link = 0;
  const uint8_t* read_ptr = input_begin;

  while (read_ptr < input_end) {
    // 1. Close the innermost message and backfill its length. 0x04 is a close
    // marker only at a field boundary; field parsers consume embedded bytes.
    if (*read_ptr == protozero::proto_utils::kProtoGroupEndByte) {
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
      protozero::proto_utils::WriteRedundantVarInt(
          static_cast<uint32_t>(content_size), output->data() + length_offset);
      innermost_open_link = enclosing_link;
      continue;
    }

    // 2. Parse and validate the next field tag.
    const uint8_t* const field_begin = read_ptr;
    uint64_t tag = 0;
    read_ptr = ParseVarIntWithinUint64(read_ptr, input_end, &tag);
    if (read_ptr == field_begin)
      return Reject(output, RewriteResult::kMalformedInput);

    const uint64_t field_id = protozero::proto_utils::GetTagFieldId(tag);
    const uint32_t wire_type =
        static_cast<uint32_t>(protozero::proto_utils::GetTagFieldType(tag));
    if (field_id == 0 || field_id > kMaxFieldId || wire_type >= 6)
      return Reject(output, RewriteResult::kMalformedInput);

    // 3. Replace a nested-message open marker with a tag and length slot.
    if (wire_type == protozero::proto_utils::kWireTypeStartGroup) {
      uint8_t tag_bytes[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
      uint8_t* const tag_end = protozero::proto_utils::WriteVarInt(
          protozero::proto_utils::MakeTagLengthDelimited(
              static_cast<uint32_t>(field_id)),
          tag_bytes);
      if (!AppendToOutput(tag_bytes, tag_end, output, max_output_size))
        return Reject(output, RewriteResult::kOutputTooLarge);

      if (output->size() > max_output_size ||
          kMessageLengthFieldSize > max_output_size - output->size()) {
        return Reject(output, RewriteResult::kOutputTooLarge);
      }
      const size_t length_offset = output->size();
      output->resize(length_offset + kMessageLengthFieldSize);
      memcpy(output->data() + length_offset, &innermost_open_link,
             sizeof(innermost_open_link));
      innermost_open_link = static_cast<uint32_t>(length_offset + 1);
      continue;
    }

    if (wire_type == protozero::proto_utils::kWireTypeEndGroup) {
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

        if (!AppendToOutput(field_begin, read_ptr, output, max_output_size))
          return Reject(output, RewriteResult::kOutputTooLarge);
        break;
      }
      case ProtoWireType::kFixed64:
      case ProtoWireType::kFixed32: {
        const size_t field_size =
            wire_type == static_cast<uint32_t>(ProtoWireType::kFixed64) ? 8 : 4;
        if (static_cast<size_t>(input_end - read_ptr) < field_size)
          return Reject(output, RewriteResult::kMalformedInput);
        read_ptr += field_size;

        if (!AppendToOutput(field_begin, read_ptr, output, max_output_size))
          return Reject(output, RewriteResult::kOutputTooLarge);
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

        if (!AppendToOutput(field_begin, read_ptr, output, max_output_size))
          return Reject(output, RewriteResult::kOutputTooLarge);
        break;
      }
    }
  }

  if (innermost_open_link != 0)
    return Reject(output, RewriteResult::kMalformedInput);

  return RewriteResult::kSuccess;
}

}  // namespace perfetto::tracing_v2
