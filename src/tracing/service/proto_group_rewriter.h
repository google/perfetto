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

#ifndef SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_H_
#define SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_H_

#include <stddef.h>

#include <array>
#include <optional>

#include "perfetto/ext/tracing/core/slice.h"

namespace perfetto::tracing_v2 {

enum class RewriteResult {
  // The output slice contains the packet with all groups rewritten.
  kRewritten,
  // The packet has no groups and already uses standard protobuf.
  // The caller must keep the input slices.
  kUnchanged,
  // The input has an invalid field or an unmatched end byte.
  // This result also applies if a group remains open at the packet end.
  // It also applies if the packet nests more than kMaxNestingDepth groups.
  kMalformedInput,
  // The content of a rewritten group exceeds kMaxMessageLength bytes.
  // The input can be valid.
  kGroupTooLarge,
};

// ProtoGroupRewriter converts a tracing v2 packet to length-delimited protobuf.
// TraceBufferV2 calls this rewriter when it reads each SMB v2 packet.
//
// Format
// ------
//
// Example:
//
//   field 1 {
//     field 2: 7
//   }
//
// On the wire:
//
//   proto group (hex):
//   0b              10 07       04
//   |               |           |
//   start field 1   contents    end byte
//
//   protobuf (hex):
//   0a              02          10 07
//   |               |           |
//   field 1         length 2    contents
//
// The rewriter changes group boundaries. It keeps all other bytes unchanged.
// - It clears bit 0 of the first tag byte. Wire type 3 becomes wire type 2.
// - It keeps the tag's byte count and inserts a length after the tag.
// - It removes the end byte, 0x04.
//
// Field ID 0 is invalid. Thus, 0x04 cannot be a protobuf tag at a field
// boundary.
//
// The packet is the root message. It has no start tag or end byte.
// The packet boundary ends the root message. The rewriter adds no root length.
//
// Lengths
// -------
//
// The encoded length depends on the rewritten content size:
//
//   rewritten content size  length field
//   ----------------------  ---------------------------------
//   0 to 127 bytes          1 byte
//   128 B to 256 MiB - 1    4 bytes (padded varint, as in v1)
//   256 MiB and above       does not fit: kGroupTooLarge
//
// A 1-byte length replaces the end byte.
// A 4-byte length adds 3 bytes for that group.
// See proto_utils::WriteRedundantVarInt().
//
// The rewriter validates each field before it copies that field.
// Existing length-delimited fields keep their contents unchanged.
// The rewriter checks their lengths but does not parse their contents.
//
// The rewriter does not return partial output.
// For a packet without groups, it does not copy data or allocate output.
// Each rewritten packet has its own output buffer.
//
// Nesting
// -------
//
// A packet can have at most kMaxNestingDepth groups open at the same time.
// The root message does not count. A deeper packet is kMalformedInput.
// - The limit keeps the nesting stack in a fixed array.
// - It also limits the length slots that the output reserves for open groups.
// - libprotobuf rejects more than 100 nested messages by default.
class ProtoGroupRewriter {
 public:
  static constexpr size_t kMaxNestingDepth = 128;

  // Rewrite() converts the packet in |input| to standard protobuf.
  // - On kRewritten, |*output| owns the rewritten packet in a new slice.
  // - For any other result, Rewrite() keeps |*output| unchanged.
  RewriteResult Rewrite(const Slices& input, std::optional<Slice>* output);

 private:
  // Storage for the nesting stack. Each Rewrite() call starts with an empty
  // stack. Each entry is the output offset of an open group's length slot.
  std::array<size_t, kMaxNestingDepth> open_groups_{};
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_H_
