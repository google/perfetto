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

#ifndef SRC_TRACING_V2_PROTO_REWRITER_H_
#define SRC_TRACING_V2_PROTO_REWRITER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace perfetto::tracing_v2 {

enum class RewriteResult {
  kSuccess,
  // Not a well-formed proto-group packet: a bad tag, value or length, a stray
  // close marker, or a message left open.
  kMalformedInput,
  // The rewritten packet would exceed |max_output_size|, or a nested message
  // would not fit its four-byte length field. The input itself may be valid.
  kOutputTooLarge,
};

// Rewrites one packet from tracing v2's append-only proto-group encoding to
// normal, length-delimited protobuf. For example, this nested message:
//
//   field 1 {
//     field 2: 7
//   }
//
// is encoded and rewritten as follows:
//
//   proto-group (hex):  0b 10 07 04
//                       \_/       \/
//                    start field 1  end current message
//
//   protobuf (hex):     0a 82 80 80 00 10 07
//                       \_/ \_________/ \___/
//                    field 1  length 2   contents
//
// The root message has no marker. Ordinary protobuf fields are copied
// unchanged. A standard protobuf end-group tag is not a valid proto-group
// close marker. Nesting depth is bounded only by the output size.
//
// The input must be the reader's private copy, not producer-owned shared
// memory. |max_output_size| bounds the rewritten root; each nested message
// must also fit its four-byte length field. |output| is empty unless the
// result is kSuccess.
RewriteResult RewriteProtoGroupToLengthDelimited(const uint8_t* input_begin,
                                                 const uint8_t* input_end,
                                                 std::vector<uint8_t>* output,
                                                 size_t max_output_size);

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_PROTO_REWRITER_H_
