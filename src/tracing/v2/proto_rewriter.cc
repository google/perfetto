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

#include <array>
#include <vector>

#include "perfetto/protozero/proto_utils.h"

namespace perfetto::tracing_v2 {
namespace {

// A packet nested more deeply than this is not something we produce, and the
// bound is what keeps the walk's state a fixed-size array rather than something
// a producer can grow.
constexpr size_t kMaxNestingDepth = 64;

// Protobuf field ids are 29 bits.
constexpr uint64_t kMaxFieldId = (1u << 29) - 1;

bool AppendBytes(const uint8_t* begin,
                 const uint8_t* end,
                 size_t max_output_size,
                 std::vector<uint8_t>* out) {
  const size_t size = static_cast<size_t>(end - begin);
  // Written as two comparisons rather than out->size() + size so the check
  // cannot itself overflow.
  if (out->size() > max_output_size || size > max_output_size - out->size())
    return false;
  out->insert(out->end(), begin, end);
  return true;
}

bool Fail(std::vector<uint8_t>* out) {
  out->clear();
  return false;
}

}  // namespace

bool RewriteToLengthDelimitedProto(const uint8_t* begin,
                                   const uint8_t* end,
                                   size_t max_output_size,
                                   std::vector<uint8_t>* out) {
  using protozero::proto_utils::kMessageLengthFieldSize;
  using protozero::proto_utils::ProtoWireType;

  out->clear();

  // For each open nested message, the output offset of its reserved four-byte
  // length. The terminator carries no field id, so there is no id to record and
  // nothing to match a close against beyond the depth itself.
  std::array<size_t, kMaxNestingDepth> open_size_field_offsets{};
  size_t depth = 0;
  const uint8_t* pos = begin;

  while (pos < end) {
    // The terminator is the exact byte 0x04 at a field boundary, recognized
    // before the tag is parsed: read as a tag it would be field id 0, which the
    // check below rejects anyway. A redundant multi-byte varint spelling of the
    // same value is not a terminator and falls through to that rejection.
    if (*pos == protozero::proto_utils::kNestedMessageTerminator) {
      if (depth == 0)
        return Fail(out);
      ++pos;
      const size_t size_field_offset = open_size_field_offsets[--depth];
      const size_t content_size =
          out->size() - size_field_offset - kMessageLengthFieldSize;
      if (content_size > protozero::proto_utils::kMaxMessageLength)
        return Fail(out);
      protozero::proto_utils::WriteRedundantVarInt(
          static_cast<uint32_t>(content_size), out->data() + size_field_offset);
      continue;
    }

    const uint8_t* const tag_begin = pos;
    uint64_t tag = 0;
    pos = protozero::proto_utils::ParseVarInt(pos, end, &tag);
    if (pos == tag_begin)
      return Fail(out);

    const uint64_t field_id_64 = tag >> 3;
    const uint32_t wire_type = static_cast<uint32_t>(tag & 7);
    if (field_id_64 == 0 || field_id_64 > kMaxFieldId || wire_type >= 6)
      return Fail(out);
    const uint32_t field_id = static_cast<uint32_t>(field_id_64);

    if (wire_type == protozero::proto_utils::kWireTypeStartGroup) {
      // The opening marker of the private framing. The same field id becomes an
      // ordinary length-delimited field with a redundant four-byte length,
      // filled in when the terminator closes it.
      if (depth == open_size_field_offsets.size())
        return Fail(out);
      uint8_t tag_bytes[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
      uint8_t* const tag_end = protozero::proto_utils::WriteVarInt(
          protozero::proto_utils::MakeTagLengthDelimited(field_id), tag_bytes);
      if (!AppendBytes(tag_bytes, tag_end, max_output_size, out))
        return Fail(out);
      open_size_field_offsets[depth++] = out->size();
      if (out->size() > max_output_size ||
          kMessageLengthFieldSize > max_output_size - out->size()) {
        return Fail(out);
      }
      out->resize(out->size() + kMessageLengthFieldSize);
      continue;
    }

    if (wire_type == protozero::proto_utils::kWireTypeEndGroup) {
      // A standard field-numbered end-group tag. The private framing closes
      // with the bare terminator handled above, so this cannot appear in a
      // well-formed packet and is not silently accepted as one.
      return Fail(out);
    }

    switch (static_cast<ProtoWireType>(wire_type)) {
      case ProtoWireType::kVarInt: {
        const uint8_t* const value_begin = pos;
        uint64_t value = 0;
        pos = protozero::proto_utils::ParseVarInt(pos, end, &value);
        if (pos == value_begin)
          return Fail(out);
        if (!AppendBytes(tag_begin, pos, max_output_size, out))
          return Fail(out);
        break;
      }
      case ProtoWireType::kFixed64:
      case ProtoWireType::kFixed32: {
        const size_t field_size =
            wire_type == static_cast<uint32_t>(ProtoWireType::kFixed64) ? 8 : 4;
        if (static_cast<size_t>(end - pos) < field_size)
          return Fail(out);
        pos += field_size;
        if (!AppendBytes(tag_begin, pos, max_output_size, out))
          return Fail(out);
        break;
      }
      case ProtoWireType::kLengthDelimited: {
        const uint8_t* const length_begin = pos;
        uint64_t length = 0;
        pos = protozero::proto_utils::ParseVarInt(pos, end, &length);
        if (pos == length_begin || length > static_cast<uint64_t>(end - pos))
          return Fail(out);
        pos += static_cast<size_t>(length);
        if (!AppendBytes(tag_begin, pos, max_output_size, out))
          return Fail(out);
        break;
      }
    }
  }

  // A message that never closed means the packet was truncated or the framing
  // was wrong; either way the output is not a packet.
  if (depth != 0)
    return Fail(out);
  return true;
}

}  // namespace perfetto::tracing_v2
