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

#ifndef SRC_TRACING_V2_TRACING_V2_ABI_H_
#define SRC_TRACING_V2_TRACING_V2_ABI_H_

#include <stdint.h>

#include "perfetto/ext/tracing/core/basic_types.h"

namespace perfetto {
namespace tracing_v2 {

// Experimental ABI. It initially validates the producer-side writer, ring and
// reader in one process; it is not yet a compatibility contract between SDK
// producers and traced. Version/extension negotiation, mapping initialization
// and cross-build atomic requirements must be defined before old and new
// binaries can share a ring.

// Chunks are a fixed 256 bytes initially. The version-0 header is six bytes:
//
//        +---------+---------+-------------------+-------------------+
//        | byte 0  | byte 1  |     bytes 2-3     |     bytes 4-5     |
//        +---------+---------+-------------------+-------------------+
//        | control | payload |     writer id     |   target buffer   |
//        |  byte   |  size   |                   |                   |
//        +---------+---------+-------------------+-------------------+
//        \__________ atomic state word __________/
//
// Bytes 6-255 are the payload. No C++ struct is overlaid on these bytes.
// Bytes 0-3 are accessed atomically as a little-endian state word; bytes 4-5
// are written under exclusive writer ownership and published by the state-word
// release transition.
// TODO(sashwinbalaji): choose the final chunk size from workload measurements
// before freezing the shared-memory ABI.
constexpr uint32_t kChunkSize = 256;
constexpr uint32_t kChunkHeaderSize = 6;
constexpr uint32_t kChunkPayloadSize = kChunkSize - kChunkHeaderSize;
constexpr uint32_t kChunkTargetBufferOffset = 4;

// The control byte is:
//
//      +---------+---------+---------+---------+---------+---------+---------+
//      |  bit 7  |bits 6-5 |  bit 4  |  bit 3  |  bit 2  |  bit 1  |  bit 0  |
//      +---------+---------+---------+---------+---------+---------+---------+
//      |extended | version |  needs  |  data   |continues|continues|acquired |
//      | header  |         | rewrite |  loss   |from prev| on next |   for   |
//      |         |         |         |         |  chunk  |  chunk  | writing |
//      +---------+---------+---------+---------+---------+---------+---------+
//
// Bits 0-4 are the v0 flags, bits 5-6 select the base header version, and bit
// 7 is the extended-header escape. V0 writers never set bit 7. A v0 reader
// that observes it discards the chunk without guessing its payload offset. The
// extension representation is deliberately undefined until a concrete feature
// requires it, and whatever it turns out to be it cannot add ownership states:
// those stay in the state word or come with a new version.
//
// The five v0 flags fill the flags field exactly; a sixth needs a new header
// version.
enum ChunkFlags : uint8_t {
  kFlagAcquiredForWriting = 1 << 0,
  kFlagContinuesOnNextChunk = 1 << 1,
  kFlagContinuesFromPrevChunk = 1 << 2,
  kFlagDataLoss = 1 << 3,
  kFlagNeedsRewrite = 1 << 4,
};

constexpr uint8_t kChunkFlagsMask = 0x1f;
constexpr uint8_t kChunkVersionShift = 5;
constexpr uint8_t kChunkVersionMask = 0x60;
constexpr uint8_t kChunkExtendedHeaderMask = 0x80;

// The reader must inspect the ownership flags before it can reject an unknown
// version without blocking behind a writer. Therefore mixed-version rings
// require either version-invariant ownership bits or negotiation that prevents
// incompatible producers and readers from sharing one.
// TODO(sashwinbalaji): choose and document one of those contracts before the
// first cross-process rollout.

// Version 0 is the layout above. The other three values are reserved for base
// layouts we have not designed; a v0 reader drops a chunk carrying one rather
// than guess where its payload starts. The version says which base layout is
// present, the extension bit says whether extra header bytes follow, and the
// two questions are deliberately independent.
constexpr uint8_t kChunkVersion = 0;

// Each payload is a sequence of explicitly-sized packet fragments:
//
//   [fragment_size:uint8][fragment bytes] [fragment_size:uint8][...] ...
//
// The reader stops at payload_size rather than at a sentinel, so a size of
// zero is an empty fragment and the bytes past payload_size have no meaning:
// they are whatever the previous lap left in the slot.
constexpr uint32_t kFragmentHeaderSize = 1;
constexpr uint32_t kMaxFragmentSize = kChunkPayloadSize - kFragmentHeaderSize;

struct ChunkHeader {
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  // Payload bytes committed by the writer, fragment size fields included. It
  // sits in the state word for two reasons: it tells the reader which prefix
  // belongs to the snapshot it just observed, and it makes every append change
  // that word, so a reader that copied the older prefix fails its CAS instead
  // of freeing a chunk it did not fully read.
  uint8_t payload_size = 0;
  uint8_t flags = 0;
  uint8_t version = 0;
  // Bit 7 of the control byte. Nothing emits it; the reader only recognises it
  // well enough to drop the chunk.
  bool extended_header = false;

  constexpr uint32_t ToStateWord() const {
    return static_cast<uint32_t>(
               (flags & kChunkFlagsMask) |
               ((version << kChunkVersionShift) & kChunkVersionMask) |
               (extended_header ? kChunkExtendedHeaderMask : 0)) |
           (static_cast<uint32_t>(payload_size) << 8) |
           (static_cast<uint32_t>(writer_id) << 16);
  }

  static constexpr ChunkHeader FromStateWord(uint32_t state_word,
                                             BufferID target_buffer) {
    ChunkHeader header{};
    const uint8_t control_byte = static_cast<uint8_t>(state_word);
    header.flags = control_byte & kChunkFlagsMask;
    header.version = static_cast<uint8_t>((control_byte & kChunkVersionMask) >>
                                          kChunkVersionShift);
    header.extended_header = (control_byte & kChunkExtendedHeaderMask) != 0;
    header.payload_size = static_cast<uint8_t>(state_word >> 8);
    header.writer_id = static_cast<WriterID>(state_word >> 16);
    header.target_buffer = target_buffer;
    return header;
  }
};

// The target buffer is the only header field outside the state word. It is
// immutable routing metadata that takes no part in ownership arbitration, so
// the state word does not have to spend bits on it, and it stays a whole
// 16-bit field instead of being split across the atomic boundary. A writer
// stores it while it exclusively owns a newly claimed slot; the release
// transition to a complete state publishes it along with the payload. The
// direct producer/service design may replace this initial v1 buffer index with
// a negotiated routing identifier.
inline void StoreChunkTargetBuffer(uint8_t* chunk, BufferID target_buffer) {
  chunk[kChunkTargetBufferOffset] = static_cast<uint8_t>(target_buffer);
  chunk[kChunkTargetBufferOffset + 1] =
      static_cast<uint8_t>(target_buffer >> 8);
}

inline BufferID LoadChunkTargetBuffer(const uint8_t* chunk) {
  return static_cast<BufferID>(
      static_cast<uint32_t>(chunk[kChunkTargetBufferOffset]) |
      (static_cast<uint32_t>(chunk[kChunkTargetBufferOffset + 1]) << 8));
}

// Ownership is carried by kFlagAcquiredForWriting and kFlagNeedsRewrite, and
// only the combinations below are legal. Anything else in a chunk header means
// the shared region is corrupted, and both sides drop the chunk rather than
// trust it.
//
//   1. state word == 0  (== kFreeStateWord)
//        Free. A writer claims it by CAS-ing from zero.
//   2. kFlagAcquiredForWriting
//        A writer is inside the chunk. It releases by clearing the flag and
//        publishing the new payload_size in the same CAS.
//   3. kFlagAcquiredForWriting | kFlagNeedsRewrite
//        The reader stepped over a live writer. That writer's release CAS
//        fails; it relocates its payload to a later position and frees this
//        slot.
//   4. neither bit set
//        Complete and readable. Its writer may take it back to append after
//        payload_size, never to rewrite what is below it.
//   5. kFlagNeedsRewrite with writer id 0  (== kInvalidatedChunkHeader)
//        A slot whose reservation the reader passed before any writer claimed
//        it. The late writer frees it rather than using it, which would hand
//        the reader data out of FIFO order.
//
// kFlagContinuesOnNextChunk, kFlagContinuesFromPrevChunk and kFlagDataLoss
// describe the payload rather than ownership, and may accompany any of these.
constexpr uint32_t kFreeStateWord = 0;

// A slot whose FIFO reservation the reader walked past before any writer
// claimed it. Writer id 0 is never valid, which is what makes it recognisable:
// the late writer that finally shows up frees it instead of using it, because
// using it would hand the reader data out of order.
constexpr uint32_t kInvalidatedChunkHeader =
    ChunkHeader{/*writer_id=*/0,    /*target_buffer=*/0,
                /*payload_size=*/0, kFlagNeedsRewrite,
                kChunkVersion,      /*extended_header=*/false}
        .ToStateWord();

static_assert(sizeof(WriterID) == 2,
              "The tracing v2 chunk ABI requires a 16-bit WriterID");
static_assert(sizeof(BufferID) == 2,
              "The tracing v2 chunk ABI requires a 16-bit BufferID");
static_assert(kChunkHeaderSize == 6,
              "The tracing v2 chunk header is part of the ABI");
static_assert(kChunkTargetBufferOffset + sizeof(BufferID) == kChunkHeaderSize,
              "The target buffer occupies the header bytes after the state "
              "word");
static_assert(kChunkPayloadSize <= UINT8_MAX,
              "The payload size and the fragment sizes must fit in one byte");
static_assert(kInvalidatedChunkHeader != kFreeStateWord,
              "An invalidated chunk must be distinguishable from a free one");
static_assert((kChunkFlagsMask & kChunkVersionMask) == 0 &&
                  (kChunkFlagsMask & kChunkExtendedHeaderMask) == 0 &&
                  (kChunkVersionMask & kChunkExtendedHeaderMask) == 0,
              "The three control-byte fields must not overlap");
static_assert((kChunkFlagsMask | kChunkVersionMask |
               kChunkExtendedHeaderMask) == 0xff,
              "The three control-byte fields must account for the whole byte");
static_assert((kFlagAcquiredForWriting | kFlagContinuesOnNextChunk |
               kFlagContinuesFromPrevChunk | kFlagDataLoss |
               kFlagNeedsRewrite) == kChunkFlagsMask,
              "The v0 flags fill the flags field exactly; a sixth one needs a "
              "new header version");

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_TRACING_V2_ABI_H_
