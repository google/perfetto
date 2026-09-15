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
// Protocol: RFC 0046,
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

// Chunks must hold several small fragments to amortize their header overhead.
constexpr uint32_t kMinChunkSize = 256;

// Each chunk's atomic<uint32_t> requires four-byte alignment.
constexpr uint32_t kChunkAlignmentBytes = 4;

// Minimum supported chunk count.
constexpr uint32_t kMinChunksPerRing = 2;

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
// position the reader must consume. These are logical positions:
// - They keep advancing past num_chunks across successive traversals.
// - As uint32_t counters, they roll over from UINT32_MAX to zero.
// - Both reader and writer map a position to a physical chunk before access.
//   ChunkIndex::FromPosition() performs that mapping modulo num_chunks.
//
// A writer reserves a position by advancing write_pos, then tries to claim
// its physical chunk.
//
// The number of reserved positions not yet handled by the reader is:
//
//   outstanding = uint32_t(write_pos - read_pos) <= num_chunks < 2^31
//
// - If outstanding is zero, write_pos == read_pos and the ring buffer is empty.
// - If outstanding equals num_chunks, the ring buffer is full:
//   write_pos == uint32_t(read_pos + num_chunks). Writers cannot advance
//   write_pos until the reader advances read_pos to make space.
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
// At most one outstanding reservation maps to each physical chunk. An exact
// comparison of the chunk's state word therefore determines ownership.
// A delayed claim can match a later traversal after the wrap count repeats.
// See WrapCountForPosition() for this limit.

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

// A physical chunk index. Variable names distinguish indices from positions:
// - *_idx selects a chunk in memory, from 0 to num_chunks - 1.
//   Successive positions cycle through these indices, then return to zero.
// - *_pos identifies a reservation in logical order.
//   It keeps advancing across traversals, past num_chunks.
//   It rolls over from UINT32_MAX to zero.
//
// For a ring buffer with four chunks:
//
//   chunk_pos: 0 1 2 3 4 5 6 7 8
//   chunk_idx: 0 1 2 3 0 1 2 3 0
class ChunkIndex {
 public:
  // Maps a logical position to its physical chunk in a validated ring buffer.
  //
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
  static constexpr ChunkIndex FromPosition(uint32_t chunk_pos,
                                           uint32_t num_chunks) {
    return ChunkIndex{chunk_pos & (num_chunks - 1)};
  }

  // Builds from an existing physical chunk index.
  // Uses |chunk_idx| unchanged, without applying modulo num_chunks.
  static constexpr ChunkIndex FromIndex(uint32_t chunk_idx) {
    return ChunkIndex{chunk_idx};
  }

  constexpr uint32_t value() const { return chunk_idx_; }

 private:
  explicit constexpr ChunkIndex(uint32_t chunk_idx) : chunk_idx_(chunk_idx) {}

  uint32_t chunk_idx_;
};

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
// On the first traversal, wrap_count is zero and every chunk's Free word is
// zero. A zero-filled ring buffer therefore starts with all chunks Free and
// read_pos == write_pos == 0. Later traversals use wrap_count << 16 as the
// Free word.
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
  // The writer lost data before or while filling this chunk.
  // - The flag does not identify where data was lost within the chunk.
  // - The reader discards all published fragments and reports loss.
  //   This includes complete, good packets before or after the gap.
  // - A writer may keep appending. The flag stays set for that reservation.
  //   Fragments appended to this chunk are also discarded once published.
  // This allows cached reuse after loss without forcing a new reservation.
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
//    - The chunk is ready for chunk_pos + num_chunks. That position's writer
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
// The writer's publication and the reader's rewrite request compare the entire
// BeingWritten word, including format, flags, num_fragments and WriterID.
// Only one CAS can succeed against that word. No second atomic is needed.
// If scraping wins, the writer relocates only the unpublished fragment after
// the N published fragments.
//
// A reservation allows one claim against Free(wrap_count(chunk_pos)).
// On failure, the writer must reserve a new position. The returned word can
// belong to another writer or a position beyond the one the reader consumed.
// A retry against that word can overwrite another reservation's chunk.
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
//   reader  Free CAS lost          retry immediately        unchanged
//   reader  BeingWritten CAS lost  discard copy, retry      unchanged
//   reader  Complete CAS lost      discard copy, retry      unchanged
//   reader  acknowledgement lost   protocol error, stop     unchanged
//   writer  publication CAS lost   handle rewrite or abort  unchanged
//   writer  any other lost CAS     bug, abort               unchanged
//
// Lost-CAS rows describe a failed attempt. It does not change the shared word.
// The reader retries within Drain(), subject to its per-position attempt limit.
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
// Computes the wrap from a position. It does not inspect the chunk's state.
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
// Fragment 0's size is at the end of the chunk. num_fragments specifies the
// number of published payload fragments and size varints.
//
// Publication and access:
// - The writer fills each fragment and its size entry before the release
//   transition from BeingWritten to Complete.
// - The reader acquire-loads the state word. It decodes and validates each size
//   before it copies the payload.
//
// BufferID becomes visible with the first fragment publication.
// If num_fragments is zero, the reader must not load BufferID.
//
// If kFlagDataLoss is set, the reader discards the chunk's published fragments
// without access to their payload.

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
// The fragment sizes are reversed varints at the end of the chunk.
// - Fragment 0's size ends at chunk_size.
// - Each later size is prepended at a lower address.
//
//   low address                                 high address
//   +----------+----------+----------+----------+----------+
//   | size N-1 |   ...    |  size 2  |  size 1  |  size 0  |
//   +----------+----------+----------+----------+----------+
//
// Within each size entry:
// - Each byte carries seven value bits and a continuation bit (0x80).
// - The least significant group is at the highest address.
// - The reader moves towards lower addresses until the continuation bit is
//   clear. It does not read the next, unpublished entry.

// Fragment sizes share Protozero's message-length limit: four varint bytes.
constexpr uint32_t kMaxFragmentSizeVarIntBytes = 4;

// Returns the largest supported n with n + varint_size(n) <= available_bytes.
// Returns zero if no payload byte fits.
// |available_bytes| includes both the payload and its size entry. Each size
// byte carries seven bits, so the limits below include the corresponding
// number of size bytes.
inline uint32_t MaxFragmentSizeForAvailableBytes(uint32_t available_bytes) {
  if (available_bytes <= 1)
    return 0;

  // One size byte: (2^7 - 1) + 1 = 2^7 total. Subtract one size byte.
  if (available_bytes <= (1u << 7))
    return available_bytes - 1;
  // Two size bytes: (2^14 - 1) + 2 = 2^14 + 1 total. Subtract two.
  if (available_bytes <= (1u << 14) + 1)
    return available_bytes - 2;
  // Three size bytes: (2^21 - 1) + 3 = 2^21 + 2 total. Subtract three.
  if (available_bytes <= (1u << 21) + 2)
    return available_bytes - 3;
  return std::min(
      available_bytes - 4,
      static_cast<uint32_t>(protozero::proto_utils::kMaxMessageLength));
}

inline uint32_t MaxFragmentSizeForEmptyChunk(uint32_t chunk_size) {
  // Callers use a validated ring buffer, so a size below 256 is a caller error.
  PERFETTO_CHECK(chunk_size >= kMinChunkSize);
  const uint32_t available_bytes = chunk_size - kTargetBufferPayloadOffset;
  return MaxFragmentSizeForAvailableBytes(available_bytes);
}

// Writes the size into the directory immediately before |sizes_begin| and
// returns the new directory start.
// Requires varint_size(size) writable bytes before |sizes_begin|.
// |size| must be at most protozero::proto_utils::kMaxMessageLength.
inline uint8_t* WriteFragmentSizeReversed(uint8_t* sizes_begin, uint32_t size) {
  PERFETTO_DCHECK(size <= protozero::proto_utils::kMaxMessageLength);
  // TODO(sashwinbalaji): Add proto_utils::WriteVarIntReversed to write directly
  // into the directory. This would avoid the temporary buffer and copy below.
  uint8_t encoded[kMaxFragmentSizeVarIntBytes];
  const uint8_t* const encoded_end =
      protozero::proto_utils::WriteVarInt(size, encoded);
  const size_t encoded_size = static_cast<size_t>(encoded_end - encoded);
  // Put the first varint byte at the highest address: the reader starts
  // there and reads towards lower addresses (see ReadFragmentSizeReversed()).
  for (size_t i = 0; i < encoded_size; ++i) {
    --sizes_begin;
    *sizes_begin = encoded[i];
  }
  return sizes_begin;
}

// Decodes one reversed size varint, starting just below |*sizes_cursor|.
//
// For example, 300 is stored as 02 AC from low to high address.
// The reader visits AC first, then 02:
//
//        ... | 02 | AC |  <- chunk_size
//              ^    ^
//              |    first byte read: AC, continuation bit set
//              second byte read: 02, no continuation bit, stop
//
// The reader validates the chunk in two steps:
// 1. Decode the sizes with |lower_bound| set to the payload start.
//    This keeps reads out of the header. The payload end is not yet known.
// 2. Add up the decoded sizes. The payload must end at or before the directory
//    start. Reject the chunk on overlap, without copying payload.
//
// Rejects truncated entries and entries needing more than four bytes.
// Four bytes limit the decoded size to Protozero's kMaxMessageLength.
//
// - Success returns the size, which may be zero. |*sizes_cursor| moves to the
//   last byte read, just above the next entry.
// - Failure returns std::nullopt and leaves |*sizes_cursor| unchanged.
inline std::optional<uint32_t> ReadFragmentSizeReversed(
    const uint8_t* lower_bound,
    const uint8_t** sizes_cursor) {
  PERFETTO_DCHECK(lower_bound <= *sizes_cursor);
  const uint8_t* cursor = *sizes_cursor;
  uint32_t value = 0;
  uint32_t num_bytes = 0;
  // TODO(sashwinbalaji): Benchmark alternatives:
  // - Increment a running shift by seven instead of multiplying each time.
  // - Find the terminator first, then decode forwards with shift-and-or.
  for (;;) {
    if (cursor == lower_bound || num_bytes == kMaxFragmentSizeVarIntBytes)
      return std::nullopt;
    --cursor;
    const uint8_t cur = *cursor;
    // Reading down the directory yields the least significant group first.
    const uint32_t data_bits = cur & 0x7f;
    const uint32_t shift = 7 * num_bytes;
    value |= data_bits << shift;
    ++num_bytes;
    if ((cur & 0x80) == 0)
      break;
  }

  *sizes_cursor = cursor;
  return value;
}

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_ABI_H_
