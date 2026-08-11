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

#ifndef SRC_TRACING_V2_PROTO_UNGROUPER_H_
#define SRC_TRACING_V2_PROTO_UNGROUPER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace perfetto {
namespace tracing_v2 {

// Converts the nested start/end groups in one complete TracePacket into
// length-delimited fields. Group encoding is an internal v2 transport form;
// packets emitted to the existing trace buffer remain canonical protobuf. Root
// bytes have no synthetic group wrapper. Scalar, fixed, string, bytes, and
// already-length-delimited fields are copied through. Returns false for
// malformed, mismatched, truncated, or excessively deep input. |out| is
// cleared before conversion and on failure.
//
// This parser becomes part of the untrusted service boundary when ChunkReader
// moves cross-process, so all recursion and output growth must remain bounded.
// TODO(sashwinbalaji): add a dedicated wire-format fuzzer before moving it into
// traced.
//
// |root_field_to_extract| names a root-level varint field that is removed from
// the output and returned in |*extracted| instead, so the caller can merge it
// with its own value and emit exactly one occurrence. Used for
// TracePacket.previous_packet_dropped, whose reasons are a bitmask that must be
// ORed rather than overwritten by a duplicate field. Pass 0 to disable.
bool UngroupProtoBytes(const uint8_t* begin,
                       const uint8_t* end,
                       size_t max_output_size,
                       std::vector<uint8_t>* out,
                       uint32_t root_field_to_extract = 0,
                       uint64_t* extracted = nullptr);

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_PROTO_UNGROUPER_H_
