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

#ifndef SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_REFERENCE_FOR_TESTING_H_
#define SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_REFERENCE_FOR_TESTING_H_

#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <vector>

#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/service/proto_group_rewriter.h"

namespace perfetto::tracing_v2 {

// A slow and simple rewriter, for tests only. It follows the contract in
// proto_group_rewriter.h, but shares no code with ProtoGroupRewriter:
// - The input is one contiguous buffer. There are no slices.
// - Each open group gets its own output buffer. When the group closes, its
//   tag, length and content are appended to its parent.
// - No length slot, no compaction, no fast path.
//
// Tests and the fuzzer compare ProtoGroupRewriter against it, on the same
// input, for the result and the bytes.
inline RewriteResult ReferenceRewriteForTesting(const std::vector<uint8_t>& in,
                                                std::vector<uint8_t>* out) {
  namespace pu = ::protozero::proto_utils;
  out->clear();

  // An open group: its rewritten tag, and its content so far.
  struct OpenGroup {
    std::vector<uint8_t> tag;
    std::vector<uint8_t> content;
  };
  std::vector<uint8_t> root;
  std::vector<OpenGroup> open_groups;
  bool has_group = false;
  size_t pos = 0;

  // Where the next field goes: the innermost open group, or the root.
  auto current = [&]() -> std::vector<uint8_t>& {
    return open_groups.empty() ? root : open_groups.back().content;
  };

  // A varint of at most 10 bytes. As ParseVarInt(), the tenth byte adds only
  // its low bit.
  auto read_varint = [&](uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t i = 0; i < 10 && pos < in.size(); ++i) {
      const uint8_t byte = in[pos++];
      result |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
      if (!(byte & 0x80)) {
        *value = result;
        return true;
      }
    }
    return false;
  };

  while (pos < in.size()) {
    // An end byte: append the innermost group to its parent.
    if (in[pos] == pu::kProtoGroupEndByte) {
      if (open_groups.empty())
        return RewriteResult::kMalformedInput;
      OpenGroup group = std::move(open_groups.back());
      open_groups.pop_back();
      const size_t size = group.content.size();
      if (size > pu::kMaxMessageLength)
        return RewriteResult::kGroupTooLarge;
      std::vector<uint8_t>& parent = current();
      parent.insert(parent.end(), group.tag.begin(), group.tag.end());
      if (size <= pu::kMaxOneByteMessageLength) {
        parent.push_back(static_cast<uint8_t>(size));
      } else {
        uint8_t length[pu::kMessageLengthFieldSize];
        pu::WriteRedundantVarInt(static_cast<uint32_t>(size), length);
        parent.insert(parent.end(), length, length + sizeof(length));
      }
      parent.insert(parent.end(), group.content.begin(), group.content.end());
      ++pos;
      continue;
    }

    // The tag, with a field id from 1 to 2^29 - 1.
    const size_t field_begin = pos;
    uint64_t tag = 0;
    if (!read_varint(&tag))
      return RewriteResult::kMalformedInput;
    const uint64_t field_id = tag >> 3;
    if (field_id == 0 || field_id > (1u << 29) - 1)
      return RewriteResult::kMalformedInput;

    // A start tag opens a group. It keeps its bytes, with wire type 2.
    uint64_t value = 0;
    switch (tag & 7) {
      case 3: {
        if (open_groups.size() == ProtoGroupRewriter::kMaxNestingDepth)
          return RewriteResult::kMalformedInput;
        OpenGroup group;
        group.tag.assign(in.begin() + static_cast<ptrdiff_t>(field_begin),
                         in.begin() + static_cast<ptrdiff_t>(pos));
        group.tag[0] &= 0xfe;
        open_groups.push_back(std::move(group));
        has_group = true;
        continue;
      }
      case 0:
        if (!read_varint(&value))
          return RewriteResult::kMalformedInput;
        break;
      case 1:
        if (in.size() - pos < 8)
          return RewriteResult::kMalformedInput;
        pos += 8;
        break;
      case 5:
        if (in.size() - pos < 4)
          return RewriteResult::kMalformedInput;
        pos += 4;
        break;
      case 2:
        if (!read_varint(&value) || value > in.size() - pos)
          return RewriteResult::kMalformedInput;
        pos += static_cast<size_t>(value);
        break;
      default:
        return RewriteResult::kMalformedInput;
    }

    // An ordinary field is copied as it is.
    std::vector<uint8_t>& dst = current();
    dst.insert(dst.end(), in.begin() + static_cast<ptrdiff_t>(field_begin),
               in.begin() + static_cast<ptrdiff_t>(pos));
  }

  if (!open_groups.empty())
    return RewriteResult::kMalformedInput;
  if (!has_group)
    return RewriteResult::kUnchanged;
  *out = std::move(root);
  return RewriteResult::kRewritten;
}

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_REFERENCE_FOR_TESTING_H_
