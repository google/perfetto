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

#include "src/tracing/v2/proto_ungrouper.h"

#include <array>

#include "perfetto/protozero/proto_utils.h"

namespace perfetto {
namespace tracing_v2 {

namespace {

constexpr size_t kMaxNestingDepth = 64;
constexpr uint64_t kMaxFieldId = (1u << 29) - 1;

struct OpenGroup {
  size_t size_field_offset = 0;
  uint32_t field_id = 0;
};

bool AppendBytes(const uint8_t* begin,
                 const uint8_t* end,
                 size_t max_output_size,
                 std::vector<uint8_t>* out) {
  const size_t size = static_cast<size_t>(end - begin);
  if (out->size() > max_output_size || size > max_output_size - out->size()) {
    return false;
  }
  out->insert(out->end(), begin, end);
  return true;
}

bool AppendVarInt(uint64_t value,
                  size_t max_output_size,
                  std::vector<uint8_t>* out) {
  uint8_t data[protozero::proto_utils::kMaxSimpleFieldEncodedSize];
  uint8_t* const end = protozero::proto_utils::WriteVarInt(value, data);
  return AppendBytes(data, end, max_output_size, out);
}

bool Fail(std::vector<uint8_t>* out) {
  out->clear();
  return false;
}

}  // namespace

bool UngroupProtoBytes(const uint8_t* begin,
                       const uint8_t* end,
                       size_t max_output_size,
                       std::vector<uint8_t>* out,
                       uint32_t root_field_to_extract,
                       uint64_t* extracted) {
  using protozero::proto_utils::kMessageLengthFieldSize;
  using protozero::proto_utils::ProtoWireType;

  out->clear();
  std::array<OpenGroup, kMaxNestingDepth> open_groups{};
  size_t depth = 0;
  const uint8_t* pos = begin;

  while (pos < end) {
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
      if (depth == open_groups.size())
        return Fail(out);
      if (!AppendVarInt(
              protozero::proto_utils::MakeTagLengthDelimited(field_id),
              max_output_size, out)) {
        return Fail(out);
      }
      open_groups[depth++] = OpenGroup{out->size(), field_id};
      if (out->size() > max_output_size ||
          kMessageLengthFieldSize > max_output_size - out->size()) {
        return Fail(out);
      }
      out->resize(out->size() + kMessageLengthFieldSize);
      continue;
    }

    if (wire_type == protozero::proto_utils::kWireTypeEndGroup) {
      if (depth == 0)
        return Fail(out);
      const OpenGroup group = open_groups[--depth];
      if (field_id != group.field_id)
        return Fail(out);
      const size_t content_size =
          out->size() - group.size_field_offset - kMessageLengthFieldSize;
      if (content_size > protozero::proto_utils::kMaxMessageLength)
        return Fail(out);
      protozero::proto_utils::WriteRedundantVarInt(
          static_cast<uint32_t>(content_size),
          out->data() + group.size_field_offset);
      continue;
    }

    switch (static_cast<ProtoWireType>(wire_type)) {
      case ProtoWireType::kVarInt: {
        const uint8_t* const value_begin = pos;
        uint64_t value = 0;
        pos = protozero::proto_utils::ParseVarInt(pos, end, &value);
        if (pos == value_begin)
          return Fail(out);
        // Hoist the caller's field out rather than copying it through, so the
        // canonical single occurrence can be emitted afterwards.
        if (depth == 0 && root_field_to_extract != 0 &&
            field_id == root_field_to_extract) {
          if (extracted)
            *extracted |= value;
          break;
        }
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

  if (depth != 0)
    return Fail(out);
  return true;
}

}  // namespace tracing_v2
}  // namespace perfetto
