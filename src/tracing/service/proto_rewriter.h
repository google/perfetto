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

#ifndef SRC_TRACING_SERVICE_PROTO_REWRITER_H_
#define SRC_TRACING_SERVICE_PROTO_REWRITER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace perfetto::tracing_v2 {

enum class RewriteResult {
  kSuccess,
  // The input has an invalid tag, value or length, an unmatched closing byte,
  // or a nested message without a closing byte.
  kMalformedInput,
  // The rewritten packet would exceed |max_output_size|, or a nested message
  // would not fit its four-byte length field. The input itself may be valid.
  kOutputTooLarge,
};

// Rewrites one packet from tracing v2's append-only proto group encoding to
// standard length-delimited protobuf. For example, this nested message:
//
//   field 1 {
//     field 2: 7
//   }
//
// is encoded and rewritten as follows:
//
//   proto group (hex):
//   0b              10 07       04
//   |               |           |
//   start field 1   contents    end current message
//
//   protobuf (hex):
//   0a              82 80 80 00               10 07
//   |               |                         |
//   field 1         length 2 (four bytes)     contents
//
// The root message has no marker. The rewriter copies ordinary protobuf fields
// unchanged. A standard protobuf end-group tag is invalid in proto group.
// Each nested message requires a tag and four length bytes in the output,
// so the output size also bounds nesting depth.
//
// Caller requirements:
// - Input must be a private copy, outside producer-owned shared memory,
//   because the producer can change bytes between validation and copy.
// - Input must not overlap |output|'s storage, which this function clears.
// - |max_output_size| must be <= UINT32_MAX because nesting links use 32-bit
//   output offsets. A larger limit fails a CHECK.
//
// The rewriter limits the entire output to |max_output_size|. Each nested
// message must also fit its four-byte length field. Either limit can produce
// kOutputTooLarge even if the input is valid.
// On failure, |output| is empty. On success, it contains the rewritten packet.
RewriteResult RewriteProtoGroupToLengthDelimited(const uint8_t* input_begin,
                                                 const uint8_t* input_end,
                                                 std::vector<uint8_t>* output,
                                                 size_t max_output_size);

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_SERVICE_PROTO_REWRITER_H_
