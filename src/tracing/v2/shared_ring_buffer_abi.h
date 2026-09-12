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
#include <string.h>

#include <algorithm>
#include <atomic>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto::tracing_v2 {

// Shared-memory ABI for a tracing-v2 producer ring buffer.
// Protocol and alternatives: RFC 0046,
// https://github.com/google/perfetto/discussions/7120.
// Parent design: RFC 0014, https://github.com/google/perfetto/discussions/4508.
//
// Several trace writers can write to the ring buffer at once. One reader drains
// their data in reservation order. A relocated suffix obtains a later
// reservation.
//
// The ring buffer has one header followed by fixed-size chunks. The header
// holds the read and write positions. Every chunk starts with one atomic state
// word. It records who may access the chunk and, while a writer owns it, how
// many complete fragments have been published.
//
// The ABI assumes little-endian producer and service processes.

// Shared-memory layout
// --------------------
//
//   +------------------------+---------+---------+---------+-----+
//   | RingBufferHeader, 64 B | chunk 0 | chunk 1 | chunk 2 | ... |
//   +------------------------+---------+---------+---------+-----+
//   0                        64

// Chunks must be large enough to hold several small fragments and amortize
// their header. RFC 0014 sets the minimum at 256 bytes.
constexpr uint32_t kMinChunkSize = 256;

// Contiguous chunks must keep every 32-bit state word aligned.
constexpr uint32_t kChunkAlignmentBytes = 4;

// Ring buffer header
// ------------------
//
//   byte offset
//   0              4              8             12               64
//   +--------------+--------------+--------------+----------------+
//   |   read_pos   |  write_pos   | num_writers_ |    reserved    |
//   |              |              |   waiting    |                |
//   +--------------+--------------+--------------+----------------+
//   \_________ rw_positions _____/ \_  atomic32 _/
//             atomic<uint64_t>
//
// The first four bytes of rw_positions contain read_pos. The following four
// bytes contain write_pos. Writers load both positions from the same atomic,
// so a capacity check cannot combine values read at different times.
//
// The reader is the only one that moves read_pos. It publishes a new value once
// per drain pass and then wakes any writer parked on a full ring buffer.
// read_pos is also the first four bytes of rw_positions, which is the address
// the futex waits on.
//
// num_writers_waiting lets the reader skip a futex wake when nobody is waiting
// for space. It is only an optimization and never decides whether the ring
// buffer is full or who owns a chunk.
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

// Logical positions and chunk indexing
// ------------------------------------
//
// write_pos is the next position a writer can reserve. read_pos is the next
// position the reader must consume. Both counters are uint32_t and wrap at
// UINT32_MAX.
//
// This is different from traversing the ring buffer: each position
// is mapped to a physical chunk by ChunkIndex::FromPosition() when that chunk
// is accessed.
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
// positions do not describe a valid ring buffer state.
//
// This also means that at most one outstanding reservation maps to each
// physical chunk (the one exception is the wrap-count alias described at
// WrapCountForPosition()), which is why an exact-value compare-and-swap on the
// chunk's state word is enough to arbitrate ownership.

// The unsigned subtraction above is unambiguous only while fewer than 2^31
// positions are outstanding. num_chunks is a power of two, so 2^30 is the
// largest legal chunk count.
constexpr uint32_t kMaxChunksPerRing = 1u << 30;

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

constexpr uint32_t NumOutstandingPositions(uint32_t write_pos,
                                           uint32_t read_pos) {
  return write_pos - read_pos;
}

// A physical chunk index, distinct from the logical reservation position.
// Variables use _idx for physical chunk indexes and _pos for logical positions.
// - _idx identifies a physical chunk in the shared memory mapping, in the range
//   [0, num_chunks). Successive logical positions cycle through these chunks,
//   returning to chunk index 0 after chunk index num_chunks - 1.
// - _pos identifies a logical position in the reservation order. It keeps
//   advancing past num_chunks across successive traversals of the ring buffer;
//   as a uint32_t, it wraps from UINT32_MAX to 0. FromPosition() maps it to the
//   physical chunk index used for memory access.
class ChunkIndex {
 public:
  // Builds from an already computed physical chunk index.
  static constexpr ChunkIndex FromIndex(uint32_t chunk_idx) {
    return ChunkIndex{chunk_idx};
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
  // For example, an eight-chunk ring buffer has three chunk-index bits. The low
  // three bits select chunks 0 through 7. The remaining 29 bits count completed
  // traversals.
  static constexpr ChunkIndex FromPosition(uint32_t chunk_pos,
                                           uint32_t num_chunks) {
    return ChunkIndex{chunk_pos & (num_chunks - 1)};
  }

  constexpr uint32_t value() const { return chunk_idx_; }

 private:
  explicit constexpr ChunkIndex(uint32_t chunk_idx) : chunk_idx_(chunk_idx) {}

  uint32_t chunk_idx_;
};

// Chunk state word
// ----------------
//
// There are five defined states: Free, BeingWritten, Complete,
// RewriteRequested and RewriteAcknowledged. The low control byte contains the
// state. It also determines how to interpret the other three bytes: Free uses
// them differently from the three data-bearing states.
//
// A Free word contains:
//
//   +-------------------+-------------------+---------------------------+
//   |      byte 0       |      byte 1       |         bytes 2-3         |
//   +-------------------+-------------------+---------------------------+
//   |    control = 0    | num_fragments = 0 |        wrap_count         |
//   | (Free, format 0,  |                   |                           |
//   |     no flags)     |                   |                           |
//   +-------------------+-------------------+---------------------------+
//          8 bits              8 bits                  16 bits
//
// A Free word is wrap_count << 16, making a zero-filled ring buffer valid and
// empty.
//
// BeingWritten, Complete and RewriteRequested contain:
//
//   +-------------------+-------------------+---------------------------+
//   |      byte 0       |      byte 1       |         bytes 2-3         |
//   +-------------------+-------------------+---------------------------+
//   |   control byte    |   num_fragments   |         WriterID          |
//   +-------------------+-------------------+---------------------------+
//          8 bits              8 bits                  16 bits
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
  // the chunk. It may take the chunk back by changing it to BeingWritten
  // before the reader reclaims it.
  kComplete = 2,

  // While the writer was appending fragment N + 1, the reader consumed the
  // first N fragments. The writer must move fragment N + 1 before releasing
  // this chunk.
  kRewriteRequested = 3,

  // After noticing RewriteRequested, the writer moves its unfinished fragment
  // and changes the old chunk to RewriteAcknowledged. The reader changes it to
  // Free when it encounters that chunk on a later traversal.
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

enum PayloadFlags : uint32_t {
  // The writer dropped trace data before writing this chunk.
  kFlagDataLoss = 1u << kPayloadFlagsShift,

  // The last fragment is not the end of its packet; the packet continues in
  // this writer's next chunk. Only Complete may carry this flag, and the
  // writer must not reuse a chunk carrying it.
  kFlagContinuesOnNextChunk = 1u << (kPayloadFlagsShift + 1),

  // The first fragment contains the next part of a packet that started in this
  // writer's previous chunk.
  kFlagContinuesFromPrevChunk = 1u << (kPayloadFlagsShift + 2),
};

// Chunk ownership and wrap identity
// ---------------------------------
//
// Only the reader writes Free. The writer publishes each closed fragment and
// may reuse its Complete chunk to append another. If the reader reaches a
// BeingWritten chunk, it takes the published prefix without waiting for the
// writer. The writer must then relocate its unpublished suffix.
//
//                writer claims               writer publishes
//   Free(wrap)  -------------->  BeingWritten(N)  -------------->  Complete(M)
//       |                              |                               |
//       | reader                       | reader                        | reader
//       v                              v                               v
//   Free(next wrap)           RewriteRequested(N)               Free(next wrap)
//                                      |
//                                      | writer finishes with old chunk
//                                      v
//                             RewriteAcknowledged
//                                      |
//                                      | reader, on a later traversal
//                                      v
//                               Free(next wrap)
//
// N and M count published fragments. A claim starts at N = 0; reuse takes
// Complete(M) back to BeingWritten(M).
//
// Publication and scraping race on the same BeingWritten word, including its
// fields. The winning CAS settles ownership; no second atomic is needed.
// If scraping wins, the writer relocates only the suffix after N fragments.
//
// A reservation allows one claim against Free(wrap_count(chunk_pos)). On
// failure, the writer must reserve a new position, never retry the new word.
//
//   Actor   From                   Action                   To
//   ------  ---------------------  -----------------------  -------------------
//   writer  Free(wrap)             claim                    BeingWritten(0)
//   writer  Free(wrap) gone        hole, reserve later      unchanged
//   reader  Free(wrap)             consume unclaimed        Free(next wrap)
//   reader  Free(other wrap)       protocol error, stop     unchanged
//   writer  BeingWritten(N)        publish                  Complete(M)
//   reader  BeingWritten(N)        take published prefix    RewriteRequested(N)
//   writer  Complete(N)            reuse                    BeingWritten(N)
//   writer  Complete gone          drop cached handle       unchanged
//   reader  Complete               consume                  Free(next wrap)
//   writer  RewriteRequested       move suffix, release     RewriteAcknowledged
//   reader  RewriteRequested       skip as a hole           unchanged
//   reader  RewriteAcknowledged    reclaim                  Free(next wrap)
//   reader  reserved state 5..7    unknown owner, stop      unchanged
//   reader  Free CAS lost          retry later              unchanged
//   reader  BeingWritten CAS lost  discard copy, retry      unchanged
//   reader  Complete CAS lost      discard copy, retry      unchanged
//   reader  acknowledgement lost   protocol error, stop     unchanged
//   writer  publication CAS lost   handle rewrite or abort  unchanged
//   writer  any other lost CAS     bug, abort               unchanged
//
// Lost-CAS rows describe a failed attempt; it does not change the shared word.
// A failed publication must find RewriteRequested(N), or the writer aborts.
// The reader also stops on reserved bits in Free or RewriteAcknowledged.
//
// Free stores the low 16 bits of the traversal number:
//
//   wrap_count = uint16_t(chunk_pos / num_chunks)
//
// For the same chunk, that value repeats after:
//
//   min(num_chunks * 65536, 2^32) reservations
//
// A writer delayed between reservation and claim for that long can match a
// later Free word. Its data is delivered at that later position, whose writer
// loses the claim. Chunk ownership holds, but reservation order is lost for
// the delayed writer. This is the limit of the 16-bit wrap count.
//
// Computes the wrap from a position; it does not inspect the chunk's state.
inline uint16_t WrapCountForPosition(uint32_t chunk_pos, uint32_t num_chunks) {
  PERFETTO_DCHECK(base::IsPowerOfTwo(num_chunks));
  return static_cast<uint16_t>(chunk_pos >> base::CountTrailZeros(num_chunks));
}

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

// The Free word a reservation at |chunk_pos| must find, and the word the
// reader leaves for the next traversal when called with chunk_pos + num_chunks.
inline uint32_t MakeFreeStateWordForPosition(uint32_t chunk_pos,
                                             uint32_t num_chunks) {
  return MakeFreeStateWord(WrapCountForPosition(chunk_pos, num_chunks));
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

// Replaces only the state bits, preserving format, flags, count and WriterID.
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
// A data-bearing format-0 chunk begins with this six-byte header. The remaining
// bytes hold payload and size entries:
//
//   +---------+---------+-------------------+-------------------+
//   | byte 0  | byte 1  |     bytes 2-3     |     bytes 4-5     |
//   +---------+---------+-------------------+-------------------+
//   | control |  num    |     WriterID      |  target BufferID  |
//   |  byte   |fragments|                   |                   |
//   +---------+---------+-------------------+-------------------+
//   \__________ atomic state word __________/
//
// The rest of a format-0 chunk is laid out as follows:
//
//   low address                                                high address
//   0                 4          6                              chunk_size
//   +-----------------+----------+-----------+------+--------------+
//   | atomic state    | BufferID | payloads  | free | size varints  |
//   | word            |          | grow ---> |      | <--- grow    |
//   +-----------------+----------+-----------+------+--------------+
//                                                     ... N-1  1  0
//
// Fragment 0's size is stored at the end of the chunk. num_fragments publishes
// the same number of payload fragments and size varints. The writer fills
// those bytes before transitioning out of BeingWritten. The reader loads the
// state word, then decodes and checks every size before copying the payload.
// The first publication also makes BufferID visible;
// the reader must not load BufferID when num_fragments is zero.

constexpr uint32_t kTargetBufferIdOffset = 4;
constexpr uint32_t kTargetBufferPayloadOffset = 6;

inline void StoreTargetBufferID(uint8_t* chunk, BufferID buffer_id) {
  memcpy(&chunk[kTargetBufferIdOffset], &buffer_id, sizeof(buffer_id));
}

inline BufferID LoadTargetBufferID(const uint8_t* chunk) {
  BufferID buffer_id;
  memcpy(&buffer_id, &chunk[kTargetBufferIdOffset], sizeof(buffer_id));
  return buffer_id;
}

// Fragment size directory
// -----------------------
//
// The fragment sizes are reverse-encoded varints at the end of the chunk. The
// first fragment's varint ends at chunk_size. Each later varint is prepended
// below the previous one:
//
//   low address                                 high address
//   +----------+----------+----------+----------+----------+
//   | size N-1 |   ...    |  size 2  |  size 1  |  size 0  |
//   +----------+----------+----------+----------+----------+
//
// The reader starts at the end of the chunk and walks towards lower addresses.
// WriteFragmentSizeReversed() mirrors each varint's bytes so the reader sees
// its least-significant group first and stops at its final byte. It never has
// to inspect the next, unpublished entry.

// Each varint byte carries seven value bits. Its top bit is set when another
// byte follows.
constexpr uint32_t kVarIntDataBitsPerByte = 7;
constexpr uint8_t kVarIntContinuationBit = 1u << kVarIntDataBitsPerByte;
constexpr uint8_t kVarIntDataBitsMask = kVarIntContinuationBit - 1;

// Fragment sizes share Protozero's message-length limit: four varint bytes.
constexpr uint32_t kMaxFragmentSizeVarIntBytes = 4;

inline uint32_t FragmentSizeVarIntByteCount(uint32_t fragment_size) {
  PERFETTO_DCHECK(fragment_size <= protozero::proto_utils::kMaxMessageLength);
  if (fragment_size < (1u << 7))
    return 1;
  if (fragment_size < (1u << 14))
    return 2;
  if (fragment_size < (1u << 21))
    return 3;
  return kMaxFragmentSizeVarIntBytes;
}

// Returns the largest supported n such that n + varint_size(n) <=
// available_bytes, or zero if no payload byte fits.
inline uint32_t MaxFragmentSizeForAvailableBytes(uint32_t available_bytes) {
  if (available_bytes <= 1)
    return 0;
  // At each threshold the directory needs one more byte. Until both that
  // byte and the larger payload fit, keep the previous maximum payload.
  if (available_bytes <= (1u << 7))
    return available_bytes - 1;
  if (available_bytes <= (1u << 14) + 1)
    return available_bytes - 2;
  if (available_bytes <= (1u << 21) + 2)
    return available_bytes - 3;
  return std::min(
      available_bytes - 4,
      static_cast<uint32_t>(protozero::proto_utils::kMaxMessageLength));
}

inline uint32_t MaxFragmentSizeForEmptyChunk(uint32_t chunk_size) {
  PERFETTO_CHECK(chunk_size >= kMinChunkSize);
  const uint32_t available_bytes = chunk_size - kTargetBufferPayloadOffset;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

// Writes the size into the directory immediately before |sizes_begin| and
// returns the new directory start. The caller must provide
// FragmentSizeVarIntByteCount(size) bytes before |sizes_begin|, and |size|
// must be at most protozero::proto_utils::kMaxMessageLength.
inline uint8_t* WriteFragmentSizeReversed(uint8_t* sizes_begin, uint32_t size) {
  PERFETTO_DCHECK(size <= protozero::proto_utils::kMaxMessageLength);
  uint8_t encoded[kMaxFragmentSizeVarIntBytes];
  const uint8_t* const encoded_end =
      protozero::proto_utils::WriteVarInt(size, encoded);
  const size_t encoded_size = static_cast<size_t>(encoded_end - encoded);
  // Put the first varint byte at the highest address: the reader starts
  // there and reads towards lower addresses (see ReadFragmentSizeReversed()).
  // TODO(sashwinbalaji): Move this into proto_utils as WriteVarIntReversed().
  for (size_t i = 0; i < encoded_size; ++i) {
    --sizes_begin;
    *sizes_begin = encoded[i];
  }
  return sizes_begin;
}

// Decodes the next size varint while moving |*sizes_cursor| towards lower
// addresses.
//
// WriteFragmentSizeReversed() mirrors each varint, so a reader walking down
// the chunk sees its bytes in protobuf decoding order. For example, a size of
// 300 is the varint AC 02 and is stored as:
//
//        ... | 02 | AC |  <- chunk_size
//              ^     ^
//              |     first byte read: AC, continuation bit set
//              second byte read: 02, no continuation bit, stop
//
// |lower_bound| is the lowest address a size byte may be read from. The caller
// uses the start of the payload because its end is known only after decoding
// all the sizes. It checks for overlap with the payload afterwards.
//
// Rejects truncated varints and encodings longer than four bytes. Non-minimal
// encodings are accepted; the writer emits the shortest representation.
//
// On success, |*sizes_cursor| points at the last byte read, which is the
// exclusive upper bound for the next size. Failure leaves the cursor unchanged.
inline std::optional<uint32_t> ReadFragmentSizeReversed(
    const uint8_t* lower_bound,
    const uint8_t** sizes_cursor) {
  const uint8_t* cursor = *sizes_cursor;
  uint32_t value = 0;
  uint32_t num_bytes = 0;
  for (;;) {
    if (cursor == lower_bound || num_bytes == kMaxFragmentSizeVarIntBytes)
      return std::nullopt;
    --cursor;
    const uint8_t byte = *cursor;
    // Reading down the directory yields the least significant group first.
    const uint32_t data_bits = byte & kVarIntDataBitsMask;
    const uint32_t shift = kVarIntDataBitsPerByte * num_bytes;
    value |= data_bits << shift;
    ++num_bytes;
    if ((byte & kVarIntContinuationBit) == 0)
      break;
  }

  *sizes_cursor = cursor;
  return value;
}

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_
