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

#include <optional>

#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/tracing/core/slice.h"

namespace perfetto::tracing_v2 {

enum class RewriteResult {
  // The output slice owns the rewritten packet.
  kRewritten,
  // The packet has no groups. The caller must keep the input slices.
  kUnchanged,
  // The input has an invalid field or unmatched group boundary.
  // This result also applies above kMaxNestingDepth.
  kMalformedInput,
  // A rewritten group exceeds proto_utils::kMaxMessageLength bytes.
  kGroupTooLarge,
};

// Converts an SMB v2 packet to length-delimited protobuf during readback.
// The rewriter replaces group boundaries with length-delimited field headers.
// It keeps the field ID, tag byte count, and all other input bytes unchanged.
//
//   Input (hex):  0b       10 07  04    field 1 { field 2: 7 }
//   Output:       0a 02    10 07
//                    ^ length
//
// Only the bare byte 0x04 closes a group. The packet has no root wrapper.
// The rewriter checks each field but does not parse length-delimited contents.
// See proto_utils::kProtoGroupEndByte for the input format.
//
// Group lengths follow the v1 encoding: one byte for contents up to 127 bytes,
// or a 4-byte padded varint for larger contents. The maximum is 256 MiB - 1.
// A packet can have at most kMaxNestingDepth open groups, excluding the root.
//
// Each rewritten packet owns its output buffer. Packets without groups need
// no copy or output allocation. The rewriter never returns partial output.
class ProtoGroupRewriter {
 public:
  // Maximum number of groups open at once.
  static constexpr size_t kMaxNestingDepth = 1024;

  // Rewrite() converts the packet in |input| to standard protobuf.
  // Each slice must be nonempty. An empty input list returns kUnchanged.
  // - On kRewritten, |*output| owns the rewritten packet in a new slice.
  // - For any other result, Rewrite() keeps |*output| unchanged.
  RewriteResult Rewrite(const Slices& input, std::optional<Slice>* output);

 private:
  // Each entry holds the output offset of an open group's length slot.
  base::SmallVector<size_t, 128> open_group_length_offsets_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_SERVICE_PROTO_GROUP_REWRITER_H_
