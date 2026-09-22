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

#ifndef INCLUDE_PERFETTO_EXT_TRACING_CORE_TRACING_V2_SHARED_RING_BUFFER_TYPES_H_
#define INCLUDE_PERFETTO_EXT_TRACING_CORE_TRACING_V2_SHARED_RING_BUFFER_TYPES_H_

#include <stdint.h>

namespace perfetto::tracing_v2 {

// A non-owning view of a trace packet's proto-group bytes, without ring-buffer
// framing. A fragment normally contains a whole packet. When a packet spans
// chunks, each chunk holds a fragment of that packet; the chunk's continuation
// flags identify whether its first or last fragment continues across chunks.
//
// The caller owns the bytes and must keep them valid and unchanged until
// admission returns.
struct Fragment {
  const uint8_t* data;
  uint32_t size;
};

// Flags retain their bit positions in the ring chunk control word.
enum PayloadFlags : uint32_t {
  // The writer lost data before or while filling this chunk.
  // - The flag does not identify where data was lost within the chunk.
  // - The reader discards all published fragments and reports loss.
  //   This includes complete, good packets before or after the gap.
  // - A writer may keep appending. The flag stays set for that reservation.
  //   Fragments appended to this chunk are also discarded once published.
  // This allows cached reuse after loss without forcing a new reservation.
  kFlagDataLoss = 1u << 5,

  // The last fragment is not the end of its packet. The packet continues in
  // this writer's next chunk. Only Complete may carry this flag, and the
  // writer must not reuse a chunk carrying it.
  kFlagContinuesOnNextChunk = 1u << (5 + 1),

  // The first fragment contains the next part of a packet that started in this
  // writer's previous chunk.
  kFlagContinuesFromPrevChunk = 1u << (5 + 2),
};

}  // namespace perfetto::tracing_v2

#endif  // INCLUDE_PERFETTO_EXT_TRACING_CORE_TRACING_V2_SHARED_RING_BUFFER_TYPES_H_
