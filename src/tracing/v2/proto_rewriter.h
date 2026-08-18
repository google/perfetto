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

// Rewrites one complete, reassembled packet from the tracing v2 private
// start-tag-and-terminator framing into ordinary length-delimited protobuf.
//
// The input framing is the one documented at
// protozero::proto_utils::kNestedMessageTerminator: a nested field f opens with
// varint((f << 3) | 3) and closes with the single byte 0x04, which carries no
// field id; the root has neither. That is deliberately not standard protobuf
// group encoding, so a field-numbered end-group tag is rejected rather than
// accepted as a close.
//
// Ordinary wire types 0, 1, 2 and 5 are copied through with bounds checks, so a
// 0x04 byte inside a length-delimited payload stays opaque. The output is valid
// length-delimited protobuf; it keeps protozero's redundant four-byte nested
// lengths rather than claiming minimal varint canonicalization.
//
// Returns false for a malformed, unmatched, truncated or excessively deep
// input, and for output that would exceed |max_output_size|. |out| is cleared
// before the conversion and again on failure, so a caller cannot mistake a
// partial rewrite for a packet.
//
// The bytes handed in are a copy the reader already took; this function never
// reads producer-owned memory. Once the reader lives in traced they become
// untrusted input from another process, which is why every bound here is
// explicit and why the nesting depth and the output size are capped.
//
// TODO(sashwinbalaji): add a wire-format fuzzer for this function before the
// consumer moves into traced.
bool RewriteToLengthDelimitedProto(const uint8_t* begin,
                                   const uint8_t* end,
                                   size_t max_output_size,
                                   std::vector<uint8_t>* out);

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_PROTO_REWRITER_H_
