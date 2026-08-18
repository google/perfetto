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

#include <atomic>

#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"

// The binary layout of the tracing-v2 producer-local shared ring buffer: the
// per-chunk atomic state word, the five ownership states, the bidirectional
// payload area and the logical-position arithmetic that ties them together.
//
// This file is pure arithmetic on a snapshot of the state word. It performs no
// atomic accesses of its own: callers load the word once and pass the value in,
// so that every ownership decision is made against one observation (see
// SharedRingBuffer and ChunkReader). Nothing here knows how a fragment's bytes
// are encoded.
//
// Ring layout:
//
//   +---------------------+---------+---------+---------+-----+
//   | ring control header | chunk 0 | chunk 1 | chunk 2 | ... |
//   +---------------------+---------+---------+---------+-----+
//
// Target-buffer chunk (format 00):
//
//   +-----------------+-----------------+-----------------------------------+
//   | state word, 4 B | target BufferID | bidirectional payload area        |
//   |                 | 2 B, little-end |                                   |
//   +-----------------+-----------------+-----------------------------------+
//    byte 0         3   4             5   6                    chunk_size - 1

namespace perfetto::tracing_v2 {

// ---------------------------------------------------------------------------
// The state word.
// ---------------------------------------------------------------------------
//
// FreeForWrap (000):
//
//   31          29 28                                             0
//  +-------------+------------------------------------------------+
//  |     000     |                29-bit wrap_count               |
//  +-------------+------------------------------------------------+
//
// Data-bearing states, i.e. Acquired (001), Complete (010),
// RewriteRequested (011):
//
//   31          29 28      27 26      24 23       16 15             0
//  +-------------+----------+----------+-----------+----------------+
//  |    state    |  format  |  flags   | num_frags |    WriterID    |
//  +-------------+----------+----------+-----------+----------------+
//
// Acknowledged (100): every bit below the state is zero.

// The top three bits of the state word. Decoding always starts here: nothing
// else in the word means anything until the state is known.
enum class ChunkState : uint32_t {
  // Vacant, and claimable only by the reservation whose wrap count is the one
  // stored in the low 29 bits.
  kFreeForWrap = 0,
  // A writer owns the chunk and may append fragments to it.
  kAcquired = 1,
  // num_fragments fragments are published and nobody is writing.
  kComplete = 2,
  // The reader has taken the first num_fragments fragments. The writer owns
  // only what is left, and is the only actor that may leave this state.
  kRewriteRequested = 3,
  // The writer has finished every access made under the old ownership. Only
  // the reader may leave this state.
  kAcknowledged = 4,
  // Reserved. A reader that meets one of these does not know who owns the
  // chunk, so there is no safe ownership move available: it stops consuming
  // that ring rather than guess.
  kReserved5 = 5,
  kReserved6 = 6,
  kReserved7 = 7,
};

constexpr uint32_t kStateShift = 29;
constexpr uint32_t kStateMask = 0x7u << kStateShift;

// Only meaningful in kFreeForWrap.
constexpr uint32_t kWrapCountMask = (1u << kStateShift) - 1;

// Only meaningful in the three data-bearing states.
constexpr uint32_t kFormatShift = 27;
constexpr uint32_t kFormatMask = 0x3u << kFormatShift;
constexpr uint32_t kPayloadFlagsShift = 24;
constexpr uint32_t kPayloadFlagsMask = 0x7u << kPayloadFlagsShift;
constexpr uint32_t kNumFragmentsShift = 16;
constexpr uint32_t kNumFragmentsMask = 0xffu << kNumFragmentsShift;
constexpr uint32_t kWriterIdMask = 0xffffu;

// The three flag bits are independent and describe the payload, not ownership.
// They are spelled as absolute bit positions because that is how the ABI is
// written down.
enum PayloadFlags : uint32_t {
  // The first fragment here is the tail of a packet that began in this
  // writer's previous chunk.
  kFlagContinuesFromPrevChunk = 1u << 26,
  // The last fragment here is the head of a packet that continues in this
  // writer's next chunk.
  kFlagContinuesOnNextChunk = 1u << 25,
  // This writer lost data before this chunk. The next packet it emits follows
  // a gap.
  kFlagDataLoss = 1u << 24,
};

// Two bits, not one, because per-packet routing is a planned chunk format
// distinction and we would rather not renumber later.
enum class ChunkFormat : uint32_t {
  // 16-bit little-endian target BufferID in bytes 4-5, payload area from byte
  // 6. This is the only format Step 1 defines.
  kTargetBuffer = 0,
  // Reserved for per-packet routing; the metadata and payload layout are for a
  // later RFC to define.
  kReservedRouting = 1,
  kReserved2 = 2,
  kReserved3 = 3,
};

constexpr ChunkState StateOf(uint32_t state_word) {
  return static_cast<ChunkState>((state_word & kStateMask) >> kStateShift);
}

// True for the three states that carry format, flags, num_fragments and a
// WriterID.
constexpr bool IsDataBearing(ChunkState state) {
  return state == ChunkState::kAcquired || state == ChunkState::kComplete ||
         state == ChunkState::kRewriteRequested;
}

// Only call after checking StateOf() == kFreeForWrap.
constexpr uint32_t WrapCountOf(uint32_t state_word) {
  return state_word & kWrapCountMask;
}

// The four accessors below are only meaningful after IsDataBearing().
constexpr ChunkFormat FormatOf(uint32_t state_word) {
  return static_cast<ChunkFormat>((state_word & kFormatMask) >> kFormatShift);
}
constexpr uint32_t PayloadFlagsOf(uint32_t state_word) {
  return state_word & kPayloadFlagsMask;
}
constexpr uint32_t NumFragmentsOf(uint32_t state_word) {
  return (state_word & kNumFragmentsMask) >> kNumFragmentsShift;
}
constexpr WriterID WriterIdOf(uint32_t state_word) {
  return static_cast<WriterID>(state_word & kWriterIdMask);
}

constexpr uint32_t MakeFreeForWrapWord(uint32_t wrap_count) {
  return wrap_count & kWrapCountMask;
}

constexpr uint32_t MakeDataBearingWord(ChunkState state,
                                       ChunkFormat format,
                                       uint32_t payload_flags,
                                       uint32_t num_fragments,
                                       WriterID writer_id) {
  return (static_cast<uint32_t>(state) << kStateShift) |
         (static_cast<uint32_t>(format) << kFormatShift) |
         (payload_flags & kPayloadFlagsMask) |
         ((num_fragments << kNumFragmentsShift) & kNumFragmentsMask) |
         writer_id;
}

constexpr uint32_t kAcknowledgedWord =
    static_cast<uint32_t>(ChunkState::kAcknowledged) << kStateShift;

// Replaces the state field and passes format, flags, num_fragments and the
// WriterID through untouched. This is what lets the reader mark a chunk whose
// format it has never heard of.
constexpr uint32_t WithState(uint32_t state_word, ChunkState state) {
  return (state_word & ~kStateMask) |
         (static_cast<uint32_t>(state) << kStateShift);
}

// ---------------------------------------------------------------------------
// Ring and chunk geometry.
// ---------------------------------------------------------------------------

constexpr uint32_t kMinChunkSize = 256;
constexpr uint32_t kMaxChunkSize = 32768;

// num_chunks is a power of two, at least 2 and strictly less than 2^31. Power
// of two so the chunk index and the wrap count are a mask and a shift; below
// 2^31 so the unsigned cursor distance is never ambiguous.
constexpr uint32_t kMinNumChunks = 2;
constexpr uint32_t kMaxNumChunks = 1u << 30;

// num_fragments is eight bits, so a chunk holds at most this many published
// fragments. A writer that reaches it closes the chunk even if payload space
// is left.
constexpr uint32_t kMaxFragmentsPerChunk = 255;

constexpr uint32_t kStateWordSize = 4;

// Format 00 only: a 16-bit little-endian BufferID at byte 4, payload from
// byte 6.
constexpr uint32_t kTargetBufferOffset = 4;
constexpr uint32_t kTargetBufferPayloadOffset = 6;

// Every size entry in a given ring is the same width, chosen once from
// chunk_size, and the choice is a constant for the life of the ring.
constexpr uint32_t FragmentSizeWidth(uint32_t chunk_size) {
  return chunk_size == kMinChunkSize ? 1 : 2;
}

constexpr uint32_t Log2ForPowerOfTwo(uint32_t value) {
  uint32_t bits = 0;
  while (bits < 31 && (1u << bits) < value)
    ++bits;
  return bits;
}

// ---------------------------------------------------------------------------
// Logical positions.
// ---------------------------------------------------------------------------
//
// read_pos and write_pos are uint32_t logical counters. They are tickets, not
// byte offsets and not chunk indices: a position identifies one reservation for
// the whole life of that reservation, even after the thread holding it has been
// asleep for a while.

// Unsigned modular subtraction, which stays exact across the uint32_t wrap.
// Never compare two positions with < or >: the answer is meaningless once one
// of them has rolled over.
constexpr uint32_t PositionDistance(uint32_t write_pos, uint32_t read_pos) {
  return write_pos - read_pos;
}

constexpr uint32_t ChunkIndexOfPosition(uint32_t position,
                                        uint32_t num_chunks) {
  return position & (num_chunks - 1);
}

constexpr uint32_t WrapCountOfPosition(uint32_t position, uint32_t chunk_bits) {
  return (position >> chunk_bits) & kWrapCountMask;
}

// The wrap count the reader exposes after resolving |position|, derived from
// the *next* logical position that maps to the same physical chunk.
//
// Away from the uint32_t cursor rollover this is the current wrap count plus
// one. At the rollover it can return to zero before all 29 bits have been used,
// which happens when the ring has more than eight chunks. That is why the
// protocol always computes this from the position and never increments the
// value found in the chunk.
constexpr uint32_t NextWrapCount(uint32_t position,
                                 uint32_t num_chunks,
                                 uint32_t chunk_bits) {
  return WrapCountOfPosition(position + num_chunks, chunk_bits);
}

// ---------------------------------------------------------------------------
// The bidirectional payload area.
// ---------------------------------------------------------------------------
//
//   low address                                             high address
//   +--------------+------------------------+--------+-------------------+
//   | chunk header | fragment payloads ---> |  free  | <--- size entries |
//   +--------------+------------------------+--------+-------------------+
//
// Payload bytes grow up from the low end of the payload area; fragment sizes
// grow down from the end of the chunk. Fragment 0's size entry is nearest the
// end of the chunk. There is no sentinel: num_fragments in the state word says
// exactly how many entries to walk.

// Byte offset, from the start of the chunk, of the |index|-th fragment's size
// entry.
constexpr uint32_t FragmentSizeEntryOffset(uint32_t chunk_size,
                                           uint32_t width,
                                           uint32_t index) {
  return chunk_size - (index + 1) * width;
}

// Multi-byte size entries are explicitly little-endian and are read and written
// byte by byte: the ring may be mapped at an address where a native 16-bit load
// would be unaligned, and we do not want to depend on host endianness either.
inline void StoreFragmentSize(uint8_t* entry, uint32_t width, uint32_t size) {
  entry[0] = static_cast<uint8_t>(size);
  if (width == 2)
    entry[1] = static_cast<uint8_t>(size >> 8);
}

inline uint32_t LoadFragmentSize(const uint8_t* entry, uint32_t width) {
  uint32_t size = entry[0];
  if (width == 2)
    size |= static_cast<uint32_t>(entry[1]) << 8;
  return size;
}

// The target BufferID is the only header field outside the state word. It is
// stored while the writer exclusively owns a freshly claimed chunk and is
// published by the release transition out of kAcquired.
inline void StoreTargetBuffer(uint8_t* chunk, BufferID target_buffer) {
  chunk[kTargetBufferOffset] = static_cast<uint8_t>(target_buffer);
  chunk[kTargetBufferOffset + 1] = static_cast<uint8_t>(target_buffer >> 8);
}

inline BufferID LoadTargetBuffer(const uint8_t* chunk) {
  return static_cast<BufferID>(
      static_cast<uint32_t>(chunk[kTargetBufferOffset]) |
      (static_cast<uint32_t>(chunk[kTargetBufferOffset + 1]) << 8));
}

// ---------------------------------------------------------------------------
// Contract checks.
// ---------------------------------------------------------------------------

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "The chunk handoff is a 32-bit CAS on the producer's hot path; a "
              "lock-backed atomic is not a handoff we can ship");
static_assert(sizeof(WriterID) == 2,
              "The state word spends its low 16 bits on a WriterID");
static_assert(sizeof(BufferID) == 2,
              "Format 00 stores a 16-bit BufferID in chunk bytes 4-5");

static_assert((kStateMask | kWrapCountMask) == 0xffffffffu &&
                  (kStateMask & kWrapCountMask) == 0,
              "FreeForWrap must split the word into exactly a state and a wrap "
              "count");
static_assert((kStateMask | kFormatMask | kPayloadFlagsMask |
               kNumFragmentsMask | kWriterIdMask) == 0xffffffffu,
              "The data-bearing fields must account for the whole word");
static_assert((kStateMask & kFormatMask) == 0 &&
                  (kFormatMask & kPayloadFlagsMask) == 0 &&
                  (kPayloadFlagsMask & kNumFragmentsMask) == 0 &&
                  (kNumFragmentsMask & kWriterIdMask) == 0,
              "The data-bearing fields must not overlap");
static_assert((kFlagContinuesFromPrevChunk | kFlagContinuesOnNextChunk |
               kFlagDataLoss) == kPayloadFlagsMask,
              "The three payload flags fill the flags field exactly; a fourth "
              "one needs a new chunk format");
static_assert(kAcknowledgedWord == 0x80000000u,
              "Acknowledged carries nothing; every bit below the state is 0");
static_assert(MakeFreeForWrapWord(0) == 0,
              "A zero-filled mapping has to read as FreeForWrap(0), which is "
              "what lets the ring skip a per-chunk stamping pass");

// The words the ABI is written down as, in hex. Checking numbers is easier than
// checking formulas.
static_assert(MakeFreeForWrapWord(5) == 0x00000005u, "");
static_assert(MakeDataBearingWord(ChunkState::kAcquired,
                                  ChunkFormat::kTargetBuffer,
                                  0,
                                  0,
                                  7) == 0x20000007u,
              "");
static_assert(MakeDataBearingWord(ChunkState::kComplete,
                                  ChunkFormat::kTargetBuffer,
                                  0,
                                  3,
                                  7) == 0x40030007u,
              "");
static_assert(MakeDataBearingWord(ChunkState::kRewriteRequested,
                                  ChunkFormat::kTargetBuffer,
                                  0,
                                  3,
                                  7) == 0x60030007u,
              "");
static_assert(MakeDataBearingWord(ChunkState::kComplete,
                                  ChunkFormat::kTargetBuffer,
                                  kFlagContinuesOnNextChunk,
                                  2,
                                  0x1234) == 0x42021234u,
              "");

static_assert(base::IsPowerOfTwo(kMinChunkSize) &&
                  base::IsPowerOfTwo(kMaxChunkSize),
              "chunk_size is a power of two in [256, 32768]");
static_assert(kMaxNumChunks < (1u << 31),
              "num_chunks must stay below 2^31 so the cursor distance is never "
              "ambiguous");
// The directory for a full chunk has to fit inside it, otherwise the fragment
// limit could not be reached even in principle.
static_assert(kMaxFragmentsPerChunk * FragmentSizeWidth(kMaxChunkSize) <
                  kMaxChunkSize - kTargetBufferPayloadOffset,
              "The largest legal directory must fit in the payload area");
// A one-byte entry is enough at 256 bytes because the payload area cannot hold
// a fragment that does not fit in a uint8_t, and a two-byte entry is enough at
// 32 KiB.
static_assert(kMinChunkSize - kTargetBufferPayloadOffset <= 0xff, "");
static_assert(kMaxChunkSize - kTargetBufferPayloadOffset <= 0xffff, "");

static_assert(Log2ForPowerOfTwo(1) == 0 && Log2ForPowerOfTwo(2) == 1 &&
                  Log2ForPowerOfTwo(8) == 3 && Log2ForPowerOfTwo(1024) == 10,
              "");

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_TRACING_V2_ABI_H_
