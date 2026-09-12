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

#include <atomic>

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
// Several trace writers can write at once. One reader drains their data in
// reservation order. A relocated fragment gets a later reservation.
//
// The ring buffer has one header followed by fixed-size chunks:
// - The header holds the read and write positions.
// - Each chunk starts with an atomic state word. It records ownership and the
//   number of published fragments while the chunk belongs to a writer.
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
//   0              4              8              12               64
//   +--------------+--------------+--------------+----------------+
//   |   read_pos   |  write_pos   | num_writers_ |    reserved    |
//   |              |              |   waiting    |                |
//   +--------------+--------------+--------------+----------------+
//   |<------- rw_positions ------>|<- atomic32 ->|
//          atomic<uint64_t>
//
// Writers check capacity by loading rw_positions once. In memory:
// - The first four bytes hold read_pos.
// - The next four bytes hold write_pos.
// Keeping both in one atomic avoids combining counters read at different times.
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
// position the reader must consume. Both counters are uint32_t and are allowed
// to wrap.
//
// A writer reserves a position by advancing write_pos, then tries to claim
// its physical chunk.
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

// num_chunks = 2^k, so the low k bits of a position select the physical chunk
// and the remaining bits count completed traversals:
//
//             bits 0..k-1                  bits k..31
//   +----------------------------+----------------------------+
//   |    physical chunk index    |      traversal number      |
//   +----------------------------+----------------------------+
//               k bits                     32 - k bits
//
// For example, an eight-chunk ring buffer has three chunk-index bits. The low
// three bits select chunks 0 through 7. The remaining 29 bits count completed
// traversals.
constexpr uint32_t ChunkIndexOf(uint32_t position, uint32_t num_chunks) {
  return position & (num_chunks - 1);
}

// Chunk state word
// ----------------
//
// Each chunk starts with a 32-bit atomic state word (std::atomic<uint32_t>).
//
// There are five states:
// 1. kFree
// 2. kBeingWritten
// 3. kComplete
// 4. kRewriteRequested
// 5. kRewriteAcknowledged
//
// The low control byte (8 bits) determines how to interpret the upper 24 bits:
// - Free has 8 unused bits that must be zero, followed by wrap_count (16 bits).
// - BeingWritten, Complete and RewriteRequested carry num_fragments (8 bits)
//   and WriterID (16 bits).
// - RewriteAcknowledged has no other fields so its upper bits are all zero.
//
// Whole-word diagrams below show bytes in increasing address order.
//
// A Free word contains:
//
//   +------------------+------------------+------------------+
//   |      byte 0      |      byte 1      |    bytes 2-3     |
//   +------------------+------------------+------------------+
//   |   control = 0    | num_fragments = 0|    wrap_count    |
//   | (Free, format 0, |                  |                  |
//   |    no flags)     |                  |                  |
//   +------------------+------------------+------------------+
//
// A Free word is wrap_count << 16, making a zero-filled ring buffer valid and
// empty.
//
// BeingWritten, Complete and RewriteRequested contain:
//
//   +------------------+------------------+------------------+
//   |      byte 0      |      byte 1      |    bytes 2-3     |
//   +------------------+------------------+------------------+
//   |   control byte   |  num_fragments   |     WriterID     |
//   +------------------+------------------+------------------+
//
// The control byte is:
//
//   +---------+---------+---------+---------+---------+
//   |bits 0-2 |bits 3-4 |  bit 5  |  bit 6  |  bit 7  |
//   +---------+---------+---------+---------+---------+
//   |  state  | format  |  data   |continues|continues|
//   |         |         |  loss   | on next |from prev|
//   +---------+---------+---------+---------+---------+
//
// RewriteAcknowledged carries no other fields so every other bit is zero.

enum class ChunkState : uint32_t {
  // No writer owns this chunk:
  // - A writer whose reserved position selects this chunk and has the same
  //   wrap count can change Free to BeingWritten before writing.
  // - To consume an unclaimed reservation, the reader must first advance the
  //   wrap count atomically. The delayed writer's claim then fails.
  kFree = 0,

  // One writer owns the chunk:
  // - The first N = num_fragments fragments are published and can be read.
  // - The writer may be appending fragment N + 1 after those published bytes.
  //
  // The writer publishes by changing BeingWritten to Complete. The reader
  // can instead copy the first N fragments and request a rewrite, without
  // waiting for the writer to finish.
  kBeingWritten = 1,

  // The writer has published all num_fragments fragments and stopped writing.
  // The reader and writer can now race to change the state:
  // - The reader consumes the fragments and changes Complete to Free.
  // - The same writer changes Complete back to BeingWritten to append more.
  // If the reader wins, the writer drops its cached handle to the chunk.
  kComplete = 2,

  // A partial chunk read won the race against the writer's publication:
  // - The reader consumed N = num_fragments and can move on.
  // - The writer still owns the chunk and may be writing fragment N + 1.
  //
  // The writer notices the request when it tries to publish. It must save
  // any unpublished fragment before acknowledging. The reader cannot free
  // the chunk until that acknowledgement.
  kRewriteRequested = 3,

  // The writer notices RewriteRequested when it next tries to publish:
  // - It saves any unpublished fragment in private memory.
  // - It changes the old chunk to RewriteAcknowledged and stops touching it.
  //
  // Acknowledgement happens before waiting for a replacement chunk.
  // Only the reader changes RewriteAcknowledged to Free, when it next visits
  // this chunk on a later traversal.
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

  // The last fragment is not the end of its packet. The packet continues in
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
// may reuse its Complete chunk to append another.
//
// The reader can make progress even when a writer is descheduled:
//
// 1. The writer pauses before claiming the chunk.
//    - The position is reserved. The chunk has not been claimed.
//    - The reader atomically changes Free(wrap) to Free(next wrap).
//      Only the wrap count changes. The state stays Free.
//    - The reader advances read_pos without delivering fragments.
//    - The chunk is ready for position + num_chunks. That position's writer
//      can claim it by changing the state to BeingWritten.
//    - The original writer resumes with the old wrap count. Its claim fails
//      without changing the chunk. It tries a new reservation.
//    - If the writer claims first, the reader's update fails instead.
//      The reader leaves read_pos unchanged and retries the position.
//
// 2. The writer pauses after claiming the chunk.
//    - The reader copies the published fragments.
//    - The reader changes BeingWritten to RewriteRequested, then advances
//      read_pos. The writer still owns the chunk.
//    - Later reservations cannot claim this chunk while it awaits
//      acknowledgement. The reader advances past those reservations without
//      changing the chunk's state word or delivering fragments.
//    - The owning writer resumes and saves any unpublished fragment.
//      Only that writer can change RewriteRequested to RewriteAcknowledged.
//    - On a later traversal, the reader changes RewriteAcknowledged to
//      Free(next wrap). It then advances read_pos without delivering fragments.
//    - The chunk is now available for a subsequent reservation.
//
// An unclaimed reservation that the reader skips is called a hole. Skipping
// means advancing read_pos without delivering fragments for that reservation.
// It can also require a state word update, as in the Free and
// RewriteAcknowledged cases above.
//
//                 writer claims                writer publishes
//     Free(wrap)  ------------> BeingWritten(N)  -----------> Complete(M)
//          |                           |                           |
//          | reader                    | reader                    | reader
//          v                           v                           v
//   Free(next wrap)           RewriteRequested(N)           Free(next wrap)
//                                      |
//                                      | writer finishes with old chunk
//                                      v
//                             RewriteAcknowledged
//                                      |
//                                      | reader, on a later traversal
//                                      v
//                               Free(next wrap)
//
// N and M count published fragments. A claim starts at N = 0. Reuse takes
// Complete(M) back to BeingWritten(M).
//
// Publication and scraping race on the same BeingWritten word, including its
// fields. The winning CAS settles ownership. No second atomic is needed.
// If scraping wins, the writer relocates only the unpublished fragment after
// the N published fragments.
//
// A reservation allows one claim against Free(wrap_count(position)). On
// failure, the writer must reserve a new position, never retry the new word.
//
//   Actor   From                   Action                   To
//   ------  ---------------------  -----------------------  -------------------
//   writer  Free(wrap)             claim                    BeingWritten(0)
//   writer  Free(wrap) gone        reserve a new position   unchanged
//   reader  Free(wrap)             consume unclaimed        Free(next wrap)
//   reader  Free(other wrap)       protocol error, stop     unchanged
//   writer  BeingWritten(N)        publish                  Complete(M)
//   reader  BeingWritten(N)        read N fragments         RewriteRequested(N)
//   writer  Complete(N)            reuse                    BeingWritten(N)
//   writer  Complete gone          drop cached handle       unchanged
//   reader  Complete               consume                  Free(next wrap)
//   writer  RewriteRequested       copy fragment, release   RewriteAcknowledged
//   reader  RewriteRequested       advance read_pos         unchanged
//   reader  RewriteAcknowledged    reclaim                  Free(next wrap)
//   reader  reserved state 5..7    unknown owner, stop      unchanged
//   reader  Free CAS lost          retry later              unchanged
//   reader  BeingWritten CAS lost  discard copy, retry      unchanged
//   reader  Complete CAS lost      discard copy, retry      unchanged
//   reader  acknowledgement lost   protocol error, stop     unchanged
//   writer  publication CAS lost   handle rewrite or abort  unchanged
//   writer  any other lost CAS     bug, abort               unchanged
//
// Lost-CAS rows describe a failed attempt. It does not change the shared word.
// A failed publication must find RewriteRequested(N), or the writer aborts.
// The reader also stops on reserved bits in Free or RewriteAcknowledged.
//
// Free stores the low 16 bits of the traversal number:
//
//   wrap_count = uint16_t(position / num_chunks)
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
// Computes the wrap from a position. It does not inspect the chunk's state.
inline uint16_t WrapCountForPosition(uint32_t position, uint32_t num_chunks) {
  PERFETTO_DCHECK(base::IsPowerOfTwo(num_chunks));
  return static_cast<uint16_t>(position >> base::CountTrailZeros(num_chunks));
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

// The Free word a reservation at |position| must find, and the word the
// reader leaves for the next traversal when called with position + num_chunks.
inline uint32_t MakeFreeStateWordForPosition(uint32_t position,
                                             uint32_t num_chunks) {
  return MakeFreeStateWord(WrapCountForPosition(position, num_chunks));
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
// while the word keeps describing the fragments it already published.
constexpr uint32_t ReplaceChunkState(uint32_t state_word, ChunkState state) {
  return (state_word & ~kChunkStateMask) |
         (static_cast<uint32_t>(state) << kChunkStateShift);
}

// Target-buffer chunk format
// --------------------------
//
// A data-bearing format-0 chunk begins with this six-byte header. The remaining
// chunk_size - 6 bytes hold payload and size entries:
//
//   +---------+---------+-------------------+-------------------+
//   | byte 0  | byte 1  |     bytes 2-3     |     bytes 4-5     |
//   +---------+---------+-------------------+-------------------+
//   | control |  num    |     WriterID      |  target BufferID  |
//   |  byte   |fragments|                   |                   |
//   +---------+---------+-------------------+-------------------+
//   \_____________ atomic state word _______/
//
// The rest of a format-0 chunk is laid out as follows:
//
//   low address                                         high address
//   0                 4          6                                 chunk_size
//   +-----------------+----------+-----------+------+--------------+
//   | atomic state    | BufferID | payloads  | free | size varints |
//   | word            |          | grow ---> |      | <--- grow    |
//   +-----------------+----------+-----------+------+--------------+
//                                                     ... N-1  1  0
//
// Fragment 0's size is stored at the end of the chunk. num_fragments publishes
// the same number of payload fragments and size varints. The writer fills
// those bytes before its release transition out of BeingWritten. The reader
// acquire-loads the state word, then decodes and checks every size before
// copying the payload. The first publication also makes BufferID visible.
// The reader must not load BufferID when num_fragments is zero.

constexpr uint32_t kTargetBufferIdOffset = 4;
constexpr uint32_t kTargetBufferPayloadOffset = 6;

inline void StoreTargetBufferId(uint8_t* chunk, BufferID buffer_id) {
  memcpy(&chunk[kTargetBufferIdOffset], &buffer_id, sizeof(buffer_id));
}

inline BufferID LoadTargetBufferId(const uint8_t* chunk) {
  BufferID buffer_id;
  memcpy(&buffer_id, &chunk[kTargetBufferIdOffset], sizeof(buffer_id));
  return buffer_id;
}

// Fragment size directory
// -----------------------
//
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

// Each varint byte carries seven value bits. Its top bit is set when another
// byte follows.
constexpr uint32_t kVarIntDataBitsPerByte = 7;
constexpr uint8_t kVarIntContinuationBit = 1u << kVarIntDataBitsPerByte;
constexpr uint8_t kVarIntDataBitsMask = kVarIntContinuationBit - 1;

// A uint32_t fragment size needs at most ceil(32 / 7) = 5 varint bytes.
constexpr uint32_t kMaxFragmentSizeVarIntBytes = 5;

constexpr uint32_t FragmentSizeVarIntByteCount(uint32_t fragment_size) {
  uint32_t bytes = 1;
  while (fragment_size >= kVarIntContinuationBit) {
    fragment_size >>= kVarIntDataBitsPerByte;
    ++bytes;
  }
  return bytes;
}

static_assert(FragmentSizeVarIntByteCount(UINT32_MAX) ==
                  kMaxFragmentSizeVarIntBytes,
              "kMaxFragmentSizeVarIntBytes must bound every uint32_t size");

// Returns the largest n such that n + varint_size(n) <= available_bytes. The
// loop runs at most four times because a uint32_t varint is at most five bytes.
constexpr uint32_t MaxFragmentSizeForAvailableBytes(uint32_t available_bytes) {
  if (available_bytes <= 1)
    return 0;
  // Leave at least one byte for the size varint.
  uint32_t fragment_size = available_bytes - 1;
  while (FragmentSizeVarIntByteCount(fragment_size) >
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

// Writes the size into the directory immediately before |sizes_begin| and
// returns the new directory start. The caller must provide
// FragmentSizeVarIntByteCount(size) bytes before |sizes_begin|.
inline uint8_t* WriteFragmentSize(uint8_t* sizes_begin, uint32_t size) {
  uint8_t encoded[kMaxFragmentSizeVarIntBytes];
  const uint8_t* const encoded_end =
      protozero::proto_utils::WriteVarInt(size, encoded);
  const size_t encoded_size = static_cast<size_t>(encoded_end - encoded);
  // Put the first varint byte at the highest address: the reader starts
  // there and reads towards lower addresses (see ReadFragmentSize()).
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
//              ^    ^
//              |    first byte read: AC, continuation bit set
//              second byte read: 02, no continuation bit, stop
//
// |lower_bound| is the lowest address a size byte may be read from, normally
// the start of the payload. It only keeps the decoder inside the chunk.
// Whether the payloads and the size bytes overlap is the caller's check, made
// once every size is decoded.
//
// Rejected, so that every size has exactly one byte pattern:
// - a varint that runs into |lower_bound| or is longer than five bytes.
// - a value above uint32_t.
// - a non-shortest encoding (81 00 for 1).
//
// On success, |*sizes_cursor| points at the last byte read, which is the
// exclusive upper bound for the next size. |*fragment_size| is also updated
// only on success.
inline bool ReadFragmentSize(const uint8_t* lower_bound,
                             const uint8_t** sizes_cursor,
                             uint32_t* fragment_size) {
  const uint8_t* cursor = *sizes_cursor;
  uint64_t value = 0;
  uint32_t num_bytes = 0;
  for (;;) {
    if (cursor == lower_bound || num_bytes == kMaxFragmentSizeVarIntBytes)
      return false;
    --cursor;
    const uint8_t byte = *cursor;
    // Reading down the directory yields the least significant group first.
    const uint64_t data_bits = byte & kVarIntDataBitsMask;
    const uint32_t shift = kVarIntDataBitsPerByte * num_bytes;
    value |= data_bits << shift;
    ++num_bytes;
    if ((byte & kVarIntContinuationBit) == 0)
      break;
  }

  // Five bytes can carry 35 value bits, hence the range check.
  if (value > UINT32_MAX)
    return false;
  // Reject non-shortest encodings so that every size has one byte pattern.
  if (FragmentSizeVarIntByteCount(static_cast<uint32_t>(value)) != num_bytes)
    return false;

  *sizes_cursor = cursor;
  *fragment_size = static_cast<uint32_t>(value);
  return true;
}

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_
