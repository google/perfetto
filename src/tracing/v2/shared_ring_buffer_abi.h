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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/public/pb_utils.h"

namespace perfetto::tracing_v2 {

// Shared-memory ABI for a tracing-v2 producer ring.
//
// Several trace writers can write to the ring at once. One reader drains their
// data in the order in which the writers reserved space.
//
// The ring has one header followed by fixed-size chunks. The header holds the
// read and write positions. Every chunk starts with one atomic state word. It
// records who may access the chunk and, while a writer owns it, how many
// complete fragments have been published.
//
// The ABI assumes little-endian producer and service processes.
//
// A few design choices, for context:
//
// - Chunks are at least 256 bytes (see kMinChunkSize for why).
// - A Free word borrows the 16-bit WriterID field for the traversal identity,
//   the "wrap count" or tag. That keeps the whole state in one atomic32. The
//   price is the alias bound described at WrapCountForPosition().
// - A writer publishes every fragment as soon as it closes it. That costs a
//   state-word transition and a size varint per fragment. In return the
//   reader can take the committed prefix of a chunk whose writer is still busy
//   in it instead of waiting for the writer.

// Shared-memory layout
// --------------------
//
//   +------------------------+---------+---------+---------+-----+
//   | RingBufferHeader, 64 B | chunk 0 | chunk 1 | chunk 2 | ... |
//   +------------------------+---------+---------+---------+-----+
//   0                        64

// Ring header
// -----------
//
//   byte offset
//   0              4              8             12               64
//   +--------------+--------------+--------------+----------------+
//   |   read_pos   |  write_pos   | num_writers_ | reserved       |
//   |              |              | waiting      |                |
//   +--------------+--------------+--------------+----------------+
//   \_________ rw_positions _____/ \_  atomic32 _/
//             atomic<uint64_t>
//
// Writers decide whether the ring has room by loading rw_positions once. The
// high half is write_pos and the low half is read_pos. Keeping them in one
// atomic prevents a capacity check from combining counters read at different
// times.
//
// The reader is the only one that moves read_pos. It publishes a new value once
// per drain pass and then wakes any writer parked on a full ring. read_pos is
// also the first four bytes of rw_positions, which is the address the futex
// waits on.
//
// num_writers_waiting lets the reader skip a futex wake when nobody is waiting
// for space. It is only an optimization and never decides whether the ring is
// full or who owns a chunk.
//
// Bytes 12..63 pad the header to one cache line.
//
static_assert(sizeof(std::atomic<uint32_t>) == 4 &&
                  alignof(std::atomic<uint32_t>) <= 4,
              "Chunk state must fit in a 4-byte-aligned ABI word");
static_assert(sizeof(std::atomic<uint64_t>) == 8,
              "The packed positions must occupy 8 bytes");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free,
              "Shared-memory atomics must be lock-free");

struct alignas(64) RingBufferHeader {
  std::atomic<uint64_t> rw_positions;
  std::atomic<uint32_t> num_writers_waiting;
  uint8_t reserved[52];
};

static_assert(offsetof(RingBufferHeader, num_writers_waiting) == 8 &&
                  offsetof(RingBufferHeader, reserved) == 12 &&
                  sizeof(RingBufferHeader) == 64,
              "RingBufferHeader does not match the shared-memory ABI");

constexpr uint64_t PackRwPositions(uint32_t write_pos, uint32_t read_pos) {
  return (static_cast<uint64_t>(write_pos) << 32) | read_pos;
}

constexpr uint32_t WritePosOf(uint64_t rw_positions) {
  return static_cast<uint32_t>(rw_positions >> 32);
}

constexpr uint32_t ReadPosOf(uint64_t rw_positions) {
  return static_cast<uint32_t>(rw_positions);
}

// Replaces the read half of a previously loaded rw_positions value, leaving
// write_pos unchanged. The reader publishes read_pos this way.
constexpr uint64_t ReplaceReadPos(uint64_t rw_positions, uint32_t read_pos) {
  return PackRwPositions(WritePosOf(rw_positions), read_pos);
}

// Logical positions and chunk indexing
// ------------------------------------
//
// write_pos is the next position a writer can reserve. read_pos is the next
// position the reader must resolve. Both counters are uint32_t and are allowed
// to wrap.
//
// The number of reserved positions not yet handled by the reader is:
//
//   outstanding = uint32_t(write_pos - read_pos)
//   outstanding <= num_chunks < 2^31
//
// Unsigned subtraction also works when write_pos wraps back to zero. For
// example:
//
//   read_pos  = UINT32_MAX - 3
//   write_pos = 2
//   uint32_t(write_pos - read_pos) = 6
//
// A legal result is at most num_chunks. A larger result means that the two
// positions do not describe a valid ring state.
//
// This also means that at most one outstanding reservation maps to each
// physical chunk (the one exception is the identity alias described at
// WrapCountForPosition()), which is why an exact-value compare-and-swap on the
// chunk's state word is enough to arbitrate ownership.

// The unsigned subtraction above is unambiguous only while fewer than 2^31
// positions are outstanding. num_chunks is a power of two, so 2^30 is the
// largest legal chunk count.
constexpr uint32_t kMaxChunksPerRing = 1u << 30;

constexpr uint32_t NumOutstandingPositions(uint32_t write_pos,
                                           uint32_t read_pos) {
  return write_pos - read_pos;
}

// num_chunks = 2^k, so the low k bits of a position select the physical chunk
// and the remaining bits count completed traversals:
//
//             bits 31..k                    bits k-1..0
//   +----------------------------+----------------------------+
//   |       traversal number     |    physical chunk index    |
//   +----------------------------+----------------------------+
//              32 - k bits                    k bits
//
// For example, an eight-chunk ring has three chunk-index bits. The low three
// bits select chunks 0 through 7. The remaining 29 bits count completed
// traversals.
constexpr uint32_t ChunkIndexOfPosition(uint32_t position,
                                        uint32_t num_chunks) {
  return position & (num_chunks - 1);
}

// 256 bytes is the initial minimum from RFC 0014. After the six-byte format-0
// header that leaves 250 bytes for payload and size varints. That is
// enough for several small fragments per chunk, which spreads the header cost
// over them, and small enough that a scrape or a relocation never copies more
// than one chunk. The SharedRingBuffer constructor rejects smaller chunks.
constexpr uint32_t kMinChunkSize = 256;

// Every chunk starts with a 32-bit atomic state word. Chunks are contiguous, so
// the chunk size must be a multiple of four bytes to keep every state word
// aligned.
constexpr uint32_t kChunkAlignmentBytes = 4;

// Chunk ownership
// ---------------
//
// The state word is also the arbitration point between the reader and a
// writer. For example, when a writer finishes while the reader is scraping the
// same chunk, they race to change the exact same BeingWritten word:
//
//   Writer: BeingWritten -> Complete
//   Reader: BeingWritten -> RewriteRequested
//
// If the writer wins, the reader sees Complete and consumes the finished
// chunk. If the reader wins, the writer sees RewriteRequested and moves only
// the data it appended after the published prefix. No second atomic is needed
// to decide which event happened first.
//
// Claiming Free is different. A writer's reservation authorizes one exact
// Free(wrap) word. If that compare-and-swap fails, the reader has already
// resolved the position or another traversal owns the chunk. The writer drops
// that reservation and must not retry against the new word.
//
// What each actor does with the word it finds, and what the word becomes, in
// the order a chunk goes through them. These are the only paths a chunk can
// move along:
//
//   Actor   From                 Action                    To
//   ------  -------------------  ------------------------  -------------------
//   writer  Free(wrap)           claim                     BeingWritten
//   writer  Free(wrap) gone      hole, reserve later       unchanged
//   reader  Free(wrap)           resolve unclaimed         Free(next wrap)
//   reader  Free(other wrap)     protocol error, stop      unchanged
//   writer  BeingWritten         publish                   Complete
//   reader  BeingWritten         resolve committed prefix  RewriteRequested
//   writer  Complete             reuse                     BeingWritten
//   writer  Complete gone        drop cached handle        unchanged
//   reader  Complete             consume                   Free(next wrap)
//   writer  RewriteRequested     move suffix, release      RewriteAcknowledged
//   reader  RewriteRequested     skip as a hole            unchanged
//   reader  RewriteAcknowledged  reclaim                   Free(next wrap)
//   reader  reserved state 5..7  unknown owner, stop       unchanged
//   reader  any, CAS lost        word moved, retry later   unchanged
//   writer  any other lost CAS   bug, abort                unchanged
//
// Only the reader writes Free.
//
// A Free word contains:
//
//    31                              16 15       8 7               0
//   +----------------------------------+-----------+------------------+
//   |            wrap_count            | num_frag- | control = 0x00   |
//   |                                  | ments = 0 | (Free, format 0, |
//   |                                  |           |  no flags)       |
//   +----------------------------------+-----------+------------------+
//                   16 bits               8 bits          8 bits
//
// A Free word is wrap_count << 16, making a zero-filled ring valid and empty.
//
// BeingWritten, Complete and RewriteRequested contain:
//
//    31                              16 15       8 7               0
//   +----------------------------------+-----------+------------------+
//   |             WriterID             |    num    |   control byte   |
//   |                                  | fragments |                  |
//   +----------------------------------+-----------+------------------+
//                   16 bits               8 bits          8 bits
//
// The control byte is:
//
//   +---------+---------+---------+---------+---------+
//   |  bit 7  |  bit 6  |  bit 5  |bits 4-3 |bits 2-0 |
//   +---------+---------+---------+---------+---------+
//   |continues|continues|  data   | format  |  state  |
//   |from prev| on next |  loss   |         |         |
//   +---------+---------+---------+---------+---------+
//
// RewriteAcknowledged carries no other fields; every other bit is zero.

enum class ChunkState : uint32_t {
  // The chunk may be claimed by the reservation with this wrap count.
  kFree = 0,

  // One writer owns the chunk. num_fragments is the prefix it has already
  // published. The writer may be appending another fragment after that prefix.
  kBeingWritten = 1,

  // The writer has published num_fragments fragments and is no longer touching
  // the chunk. It may take the chunk back before the reader reclaims it.
  kComplete = 2,

  // The reader took the published prefix while the writer still owned the
  // chunk. The writer must move anything it appended afterwards, then release
  // this chunk.
  kRewriteRequested = 3,

  // The writer has finished with the old chunk. The reader may reclaim it.
  kRewriteAcknowledged = 4,

  // A reader that does not know a state cannot tell who owns the chunk. It
  // stops rather than reclaiming it.
  kReserved5 = 5,
  kReserved6 = 6,
  kReserved7 = 7,
};

enum class ChunkFormat : uint32_t {
  // Bytes 4..5 contain a little-endian target BufferID. Payload starts at 6.
  kTargetBuffer = 0,

  // Reserved for a format carrying per-packet routing information.
  kReservedRouting = 1,
  kReserved2 = 2,
  kReserved3 = 3,
};

// Field widths and positions in the 32-bit word. The low byte is the control
// byte, followed by the fragment count and WriterID.
constexpr uint32_t kPayloadFlagsBits = 3;
constexpr uint32_t kChunkFormatBits = 2;
constexpr uint32_t kChunkStateBits = 3;
constexpr uint32_t kNumFragmentsBits = 8;
constexpr uint32_t kWriterIDBits = 16;

constexpr uint32_t kChunkStateShift = 0;
constexpr uint32_t kChunkFormatShift = kChunkStateShift + kChunkStateBits;
constexpr uint32_t kPayloadFlagsShift = kChunkFormatShift + kChunkFormatBits;
constexpr uint32_t kNumFragmentsShift = kPayloadFlagsShift + kPayloadFlagsBits;
constexpr uint32_t kWriterIDShift = kNumFragmentsShift + kNumFragmentsBits;

constexpr uint32_t kChunkStateMask = (1u << kChunkStateBits) - 1;
constexpr uint32_t kChunkFormatMask = ((1u << kChunkFormatBits) - 1)
                                      << kChunkFormatShift;
constexpr uint32_t kPayloadFlagsMask = ((1u << kPayloadFlagsBits) - 1)
                                       << kPayloadFlagsShift;
constexpr uint32_t kNumFragmentsMask = ((1u << kNumFragmentsBits) - 1)
                                       << kNumFragmentsShift;
constexpr uint32_t kWriterIDMask = ((1u << kWriterIDBits) - 1)
                                   << kWriterIDShift;

constexpr uint32_t kMaxFragmentsPerChunk = (1u << kNumFragmentsBits) - 1;

// Free uses the WriterID field for the wrap count. Its other data bits must be
// zero.
constexpr uint32_t kWrapCountShift = kWriterIDShift;

// Free stores the low 16 bits of the position's traversal number:
//
//   wrap_count = uint16_t(position / num_chunks)
//
// A writer can claim a chunk only when this value matches its reservation.
// This stops a delayed writer from claiming a Free word belonging to another
// traversal. The 16-bit identity repeats after:
//
//   min(num_chunks * 65536, 2^32) reservations
//
// A writer that gets suspended between reserving a position and claiming its
// chunk for that long will find its expected Free word matching again, so the
// late claim succeeds and its data comes out at that later position. Nothing
// gets corrupted, but reservation order is not guaranteed for that writer any
// more. We accept this as the price of a 16-bit identity.
inline uint16_t WrapCountForPosition(uint32_t position, uint32_t num_chunks) {
  PERFETTO_DCHECK(base::IsPowerOfTwo(num_chunks));
  return static_cast<uint16_t>(position >> base::CountTrailZeros(num_chunks));
}

enum PayloadFlags : uint32_t {
  // The writer dropped trace data before writing this chunk.
  kFlagDataLoss = 1u << kPayloadFlagsShift,

  // The last fragment is not the end of its packet; the packet continues in
  // this writer's next chunk.
  kFlagContinuesOnNextChunk = 1u << (kPayloadFlagsShift + 1),

  // The first fragment contains the next part of a packet that started in this
  // writer's previous chunk.
  kFlagContinuesFromPrevChunk = 1u << (kPayloadFlagsShift + 2),
};

constexpr ChunkState ChunkStateOf(uint32_t state_word) {
  return static_cast<ChunkState>((state_word & kChunkStateMask) >>
                                 kChunkStateShift);
}

constexpr bool HasDataFields(ChunkState state) {
  return state == ChunkState::kBeingWritten || state == ChunkState::kComplete ||
         state == ChunkState::kRewriteRequested;
}

constexpr uint32_t MakeFreeStateWord(uint16_t wrap_count) {
  return static_cast<uint32_t>(wrap_count) << kWrapCountShift;
}

// These accessors apply to BeingWritten, Complete and RewriteRequested.
// PayloadFlagsOf() leaves the flags in their encoded bit positions, so its
// result can be passed to MakeDataStateWord().
constexpr ChunkFormat ChunkFormatOf(uint32_t state_word) {
  return static_cast<ChunkFormat>((state_word & kChunkFormatMask) >>
                                  kChunkFormatShift);
}

constexpr uint32_t PayloadFlagsOf(uint32_t state_word) {
  return state_word & kPayloadFlagsMask;
}

constexpr uint32_t NumFragmentsOf(uint32_t state_word) {
  return (state_word & kNumFragmentsMask) >> kNumFragmentsShift;
}

constexpr WriterID WriterIDOf(uint32_t state_word) {
  return static_cast<WriterID>((state_word & kWriterIDMask) >> kWriterIDShift);
}

inline uint32_t MakeDataStateWord(ChunkState state,
                                  ChunkFormat format,
                                  uint32_t payload_flags,
                                  uint32_t num_fragments,
                                  WriterID writer_id) {
  PERFETTO_DCHECK(HasDataFields(state));
  PERFETTO_DCHECK(static_cast<uint32_t>(format) < (1u << kChunkFormatBits));
  PERFETTO_DCHECK((payload_flags & ~kPayloadFlagsMask) == 0);
  PERFETTO_DCHECK(num_fragments <= kMaxFragmentsPerChunk);
  return (static_cast<uint32_t>(state) << kChunkStateShift) |
         (static_cast<uint32_t>(format) << kChunkFormatShift) |
         (payload_flags & kPayloadFlagsMask) |
         ((num_fragments << kNumFragmentsShift) & kNumFragmentsMask) |
         (static_cast<uint32_t>(writer_id) << kWriterIDShift);
}

constexpr uint32_t kRewriteAcknowledgedStateWord =
    static_cast<uint32_t>(ChunkState::kRewriteAcknowledged) << kChunkStateShift;

// Returns |state_word| with only its state bits replaced. Format, flags,
// fragment count and WriterID stay as they are, like ReplaceReadPos() above.
// The reader uses this to request a rewrite without understanding the chunk
// format. The writer uses it to take a Complete chunk back to BeingWritten
// while the word keeps describing the prefix it already published.
constexpr uint32_t ReplaceChunkState(uint32_t state_word, ChunkState state) {
  return (state_word & ~kChunkStateMask) |
         (static_cast<uint32_t>(state) << kChunkStateShift);
}

// Target-buffer chunk format
// --------------------------
//
// A data-bearing format-0 chunk begins with this six-byte header:
//
//   +---------+---------+-------------------+-------------------+
//   | byte 0  | byte 1  |     bytes 2-3     |     bytes 4-5     |
//   +---------+---------+-------------------+-------------------+
//   | control |  num    |     WriterID      |  target BufferID  |
//   |  byte   |fragments|                   |                   |
//   +---------+---------+-------------------+-------------------+
//   \_____________ atomic state word _______/
//
// Free uses bytes 2-3 for the wrap count and requires every other data bit to
// be zero. RewriteAcknowledged carries only the control byte; bytes 1-3 are
// zero.
//
// The rest of a format-0 chunk is laid out as follows:
//
//   low address                                                high address
//   0                 4          6                    chunk_size
//   +-----------------+----------+-----------+------+--------------+
//   | atomic state    | BufferID | payloads  | free | size varints |
//   | word            |          | grow ---> |      | <--- grow    |
//   +-----------------+----------+-----------+------+--------------+
//                                                      ... N  1  0
//
// Fragment 0's size is stored at the end of the chunk. num_fragments publishes
// the same number of payload fragments and size varints. The writer fills
// those bytes before its release transition out of BeingWritten. The reader
// acquire-loads the state word, then decodes and checks every size before
// copying the payload.

constexpr uint32_t kTargetBufferIDOffset = 4;
constexpr uint32_t kTargetBufferPayloadOffset = 6;

inline void StoreTargetBufferID(uint8_t* chunk, BufferID target_buffer_id) {
  chunk[kTargetBufferIDOffset] = static_cast<uint8_t>(target_buffer_id);
  chunk[kTargetBufferIDOffset + 1] =
      static_cast<uint8_t>(target_buffer_id >> 8);
}

inline BufferID LoadTargetBufferID(const uint8_t* chunk) {
  return static_cast<BufferID>(
      static_cast<uint32_t>(chunk[kTargetBufferIDOffset]) |
      (static_cast<uint32_t>(chunk[kTargetBufferIDOffset + 1]) << 8));
}

// The fragment sizes are varints at the end of the chunk. The first fragment's
// varint ends at chunk_size. Each later varint is prepended below the previous
// one:
//
//   low address                                 high address
//   +----------+----------+----------+----------+----------+
//   | size N-1 |   ...    |  size 2  |  size 1  |  size 0  |
//   +----------+----------+----------+----------+----------+
//
// The reader walks the sizes from high addresses to low addresses. It sees the
// bytes of each size in normal protobuf varint order and stops at that
// varint's final byte. It never has to inspect the next, unpublished entry.
//
// Fragment sizes must use the shortest varint encoding. For example, 1 is
// encoded as 01, not 81 00. The reader rejects redundant encodings.

constexpr uint32_t kMaxFragmentSizeVarIntBytes = PERFETTO_PB_VARINT_MAX_SIZE_32;

// Each varint byte carries seven value bits. Its top bit is set when another
// byte follows.
constexpr uint32_t kVarIntDataBitsPerByte = 7;
constexpr uint8_t kVarIntContinuationBit = 1u << kVarIntDataBitsPerByte;
constexpr uint8_t kVarIntDataBitsMask = kVarIntContinuationBit - 1;

constexpr uint32_t FragmentSizeVarIntBytes(uint32_t fragment_size) {
  uint32_t bytes = 1;
  while (fragment_size >= kVarIntContinuationBit) {
    fragment_size >>= kVarIntDataBitsPerByte;
    ++bytes;
  }
  return bytes;
}

// Returns the largest n such that n + varint_size(n) <= available_bytes. The
// loop runs at most four times because a uint32_t varint is at most five bytes.
constexpr uint32_t MaxFragmentSizeForAvailableBytes(uint32_t available_bytes) {
  if (available_bytes <= 1)
    return 0;
  uint32_t fragment_size = available_bytes - 1;
  while (FragmentSizeVarIntBytes(fragment_size) >
         available_bytes - fragment_size) {
    --fragment_size;
  }
  return fragment_size;
}

constexpr uint32_t MaxFragmentSizeForEmptyChunk(uint32_t chunk_size) {
  if (chunk_size < kMinChunkSize)
    return 0;
  const uint32_t available_bytes = chunk_size - kTargetBufferPayloadOffset;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

// Prepends one fragment size below |sizes_begin| and returns the new lowest
// size byte. The destination moves down while the encoded bytes are copied in
// order, so a reader moving down through the sizes sees an ordinary protobuf
// varint. The caller must leave FragmentSizeVarIntBytes(fragment_size) bytes
// before |sizes_begin|.
inline uint8_t* WriteFragmentSize(uint8_t* sizes_begin,
                                  uint32_t fragment_size) {
  uint8_t encoded[kMaxFragmentSizeVarIntBytes];
  const uint8_t* const encoded_end =
      PerfettoPbWriteVarInt(fragment_size, encoded);
  const size_t encoded_size = static_cast<size_t>(encoded_end - encoded);
  for (size_t i = 0; i < encoded_size; ++i) {
    --sizes_begin;
    *sizes_begin = encoded[i];
  }
  return sizes_begin;
}

// Decodes the next size varint while moving |*sizes_cursor| towards lower
// addresses.
//
// WriteFragmentSize() stores each varint reversed, so a reader walking down
// the chunk sees the bytes in normal varint order. For example, a size of 300
// is the varint AC 02 and is stored as:
//
//        ... | 02 | AC |  <- chunk_size
//              ^     ^
//              |     first byte read: AC, continuation bit set
//              second byte read: 02, no continuation bit, stop
//
// The decoder walks down the chunk one byte at a time:
//   1) It stops if the cursor reaches |lower_bound| (the end of the payload)
//      or if five bytes have been read: the varint is truncated or too long.
//   2) It takes the next lower byte and adds its seven value bits.
//   3) A byte without the continuation bit ends the varint.
//   4) It rejects values above uint32_t and non-shortest encodings (81 00 for
//      1), so that every size has exactly one byte pattern.
//
// |*sizes_cursor| (now immediately before the varint) and |*fragment_size|
// are only updated on success.
inline bool ReadFragmentSize(const uint8_t* lower_bound,
                             const uint8_t** sizes_cursor,
                             uint32_t* fragment_size) {
  const uint8_t* cursor = *sizes_cursor;
  uint64_t value = 0;
  uint32_t num_bytes = 0;
  for (;;) {
    if (cursor == lower_bound || num_bytes == kMaxFragmentSizeVarIntBytes)
      return false;
    const uint8_t byte = *--cursor;
    value |= static_cast<uint64_t>(byte & kVarIntDataBitsMask)
             << (kVarIntDataBitsPerByte * num_bytes);
    ++num_bytes;
    if ((byte & kVarIntContinuationBit) == 0)
      break;
  }

  // Five bytes can carry 35 value bits, hence the range check.
  if (value > UINT32_MAX ||
      FragmentSizeVarIntBytes(static_cast<uint32_t>(value)) != num_bytes) {
    return false;
  }

  *sizes_cursor = cursor;
  *fragment_size = static_cast<uint32_t>(value);
  return true;
}

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_
