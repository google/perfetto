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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "src/tracing/v2/tracing_v2_abi.h"

// The one definition of "this platform has the futex", used by every
// conditional in the .cc and by the production gate, so they cannot drift
// apart. Deliberately LINUX_BUT_NOT_QNX: perfetto defines PERFETTO_OS_LINUX as
// true on QNX for historical reasons, and QNX has neither linux/futex.h nor
// SYS_futex.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_TRACING_V2_HAS_FUTEX() 1
#else
#define PERFETTO_TRACING_V2_HAS_FUTEX() 0
#endif

namespace perfetto {
namespace tracing_v2 {

// Whether the tracing v2 data path can run at all. Writers block on a futex
// when the ring fills, so a platform without one cannot host it.
constexpr bool kHasFutex = PERFETTO_TRACING_V2_HAS_FUTEX();

class SharedRingBufferTestPeer;

// Fixed-chunk multi-producer/single-consumer (MPSC) ring used by the
// producer-side tracing v2 data path. The ring never overwrites unread data.
// Writers reserve FIFO positions and publish complete chunks with a release
// transition on the first four header bytes; the sole reader copies the
// committed prefix and validates that copy by CAS-ing the exact state word it
// observed to free.
//
// This initially owns a producer-local PagedMemory allocation. The ownership
// protocol
// is intended to survive, but this storage API is not: moving the consumer into
// the service requires a non-owning ring view over a negotiated shared mapping.
// TODO(sashwinbalaji): split allocation ownership from the ring view before the
// producer and service use this across a process boundary.
class SharedRingBuffer {
 public:
  class Chunk {
   public:
    Chunk() = default;
    // No destructor on purpose: a Chunk is a bare handle and its move operator
    // sits on the write path. Same rationale as SharedMemoryABI::Chunk.
    ~Chunk() = default;

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) noexcept;
    Chunk& operator=(Chunk&&) noexcept;

    bool is_valid() const { return begin_ != nullptr; }
    bool is_being_written() const {
      return (state_word_ & kFlagAcquiredForWriting) != 0;
    }

    uint8_t* payload_begin() const {
      PERFETTO_DCHECK(is_valid());
      return begin_ + kChunkHeaderSize;
    }
    uint8_t* payload_end() const { return begin_ + kChunkSize; }
    uint32_t payload_used() const { return payload_used_; }
    uint32_t payload_free() const { return kChunkPayloadSize - payload_used_; }
    void set_payload_used(uint32_t payload_used) {
      PERFETTO_DCHECK(payload_used <= kChunkPayloadSize);
      payload_used_ = static_cast<uint8_t>(payload_used);
    }

   private:
    friend class SharedRingBuffer;
    friend class SharedRingBufferTestPeer;

    Chunk(uint8_t* begin, uint32_t state_word)
        : begin_(begin), state_word_(state_word) {}

    // The payload length the last release published for this chunk. Zero for a
    // freshly claimed one. Re-acquiring only ever appends after it.
    uint32_t committed_payload_size() const {
      return ChunkHeader::FromStateWord(state_word_, /*target_buffer=*/0)
          .payload_size;
    }

    void Reset() { *this = Chunk(); }

    // Keep this handle small: it lives in every TraceWriterV2.
    uint8_t* begin_ = nullptr;
    uint32_t state_word_ = 0;
    uint8_t payload_used_ = 0;
  };

  struct Stats {
    // TODO(sashwinbalaji): expose these counters through tracing stats or
    // metatracing before enabling the v2 path outside controlled validation.
    std::atomic<uint64_t> chunks_invalidated{0};
    std::atomic<uint64_t> chunks_rewritten{0};
    std::atomic<uint64_t> chunks_lost{0};
    std::atomic<uint64_t> malformed_chunks{0};
    // Chunks whose control byte named a header version or an extension we do
    // not implement. Not a corruption: it means a producer newer than us.
    std::atomic<uint64_t> chunks_unsupported{0};
    // How often the reader threw a copy away because the owning writer took
    // the chunk back while it was copying. A high number against chunks read
    // means the relay keeps catching writers mid-chunk.
    std::atomic<uint64_t> chunks_recopied{0};
  };

  enum class ReadResult {
    kNoData,
    kChunkRead,
    kChunkSkipped,
  };

  // |num_chunks| must be a power of two and smaller than 2^31 so unsigned
  // position differences remain unambiguous across wraparound.
  explicit SharedRingBuffer(uint32_t num_chunks);
  ~SharedRingBuffer();

  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  SharedRingBuffer(SharedRingBuffer&&) = delete;
  SharedRingBuffer& operator=(SharedRingBuffer&&) = delete;

  uint32_t num_chunks() const { return num_chunks_; }
  size_t size() const { return mem_.size(); }
  const Stats& stats() const { return stats_; }

  // Writer side. All methods are lock-free and can be called concurrently by
  // different writers. A Chunk itself remains single-writer.
  Chunk TryAcquireChunkForWriting(const ChunkHeader&);
  bool TryReacquireChunkForWriting(Chunk*);

  // Publishes |*chunk|. If the reader passed it while it was active, this
  // relocates the payload to a later position before returning. Returns false
  // only when relocation found the ring full and the payload was dropped.
  [[nodiscard]] bool ReleaseChunkAsComplete(Chunk*, uint8_t added_flags);

  // Reader side. There must be exactly one reader. |payload| must point to
  // kChunkPayloadSize bytes, of which the first header->payload_size are
  // filled in. The owning writer is free to append while this copies: it only
  // ever writes past the committed prefix, and its publication changes the
  // state word, which fails the CAS that would otherwise free the chunk.
  ReadResult TryReadChunk(ChunkHeader*, uint8_t* payload);

  uint32_t write_pos() const {
    return ring_buffer_header()->write_pos.load(std::memory_order_acquire);
  }
  uint32_t read_pos() const {
    return ring_buffer_header()->read_pos.load(std::memory_order_acquire);
  }
  bool has_pending_data() const { return read_pos() != write_pos(); }

  // Unsigned wraparound comparison, for the 2^32 rollover of the two cursors.
  // Valid because the ring keeps the two positions less than 2^31 apart.
  static bool IsPositionAtOrAfter(uint32_t position, uint32_t reference) {
    return position - reference < (1u << 31);
  }

  // A writer that found the ring full reads this *before* its last acquisition
  // attempt and passes it to WaitForReaderProgress(), so that progress made in
  // between fails the wait immediately rather than being missed.
  uint32_t reader_generation() const {
    return ring_buffer_header()->reader_generation.load(
        std::memory_order_acquire);
  }

  // Blocks until the reader frees chunks, |timeout_ms| elapses, or the wait is
  // woken spuriously. Callers must re-check their own predicate in a loop.
  //
  // Backed by the Linux/Android futex, the only two platforms targeted so far;
  // reaching
  // this method elsewhere is fatal even in release builds.
  // TODO(sashwinbalaji): replace with cross-platform sh_futex.
  void WaitForReaderProgress(uint32_t last_generation, uint32_t timeout_ms);

  // Publishes the chunks freed since the last call and wakes stalled writers.
  // Call once per drain pass, not once per chunk.
  void NotifyReaderProgress();

 private:
  friend class SharedRingBufferTestPeer;

  // Monotonically wrapping logical positions: write_pos counts positions
  // *reserved* and read_pos counts positions *consumed*. Neither counts
  // payload-bearing chunks. A position is reserved before its writer claims
  // the physical chunk, and the reader consumes a position whether it found a
  // complete chunk there, a live writer it had to step over, or an invalidated
  // hole whose writer never arrived. That distinction is what the delayed
  // writer, wraparound and publish-before-claim reasoning below all turn on.
  //
  // These are positions rather than offsets: the physical slot is
  // |pos & chunk_index_mask_|. Keeping them unmasked is what lets "full" be
  // write_pos - read_pos == num_chunks and keeps all N slots usable. A pair of
  // pre-masked offsets would have to sacrifice one slot to distinguish full
  // from empty.
  //
  // Split by who writes what, one cache line each, so the two sides do not
  // ping-pong a line: every writer thread CASes write_pos, and only the reader
  // stores read_pos.
  struct alignas(64) RingBufferHeader {
    // Written by writers.
    std::atomic<uint32_t> write_pos{0};
    // How many writers are inside WaitForReaderProgress(). Lets the reader
    // skip the wake syscall when nobody is stalled.
    std::atomic<uint32_t> num_writers_waiting{0};
    // The two atomics above occupy eight bytes; 56 bytes finish their 64-byte
    // cache line so writer CAS traffic does not invalidate the reader-owned
    // fields below.
    uint8_t writer_cache_line_padding[56]{};

    // Written by the reader.
    std::atomic<uint32_t> read_pos{0};
    // The futex word. Bumped every time the reader frees chunks. Writers wait
    // on it rather than on read_pos, because a futex needs a value that only
    // ever changes when there is something to wake up for.
    std::atomic<uint32_t> reader_generation{0};
    // Keep the reserved area on a new cache line. This padding is not space for
    // future control fields: adding one here could reintroduce false sharing.
    uint8_t reader_cache_line_padding[56]{};

    // For the cross-process control fields chosen in later steps.
    //
    // TODO(sashwinbalaji): add durable ring-wide data-loss auditing before
    // field validation. kFlagDataLoss can report an exhausted ring only after
    // that writer successfully publishes another chunk; if it never recovers,
    // the loss is invisible to the consumer. Decide between a saturating
    // shared counter here and an equivalently durable per-writer mechanism,
    // and place it so increments do not add contention to the cursor lines.
    uint8_t reserved[128]{};
  };
  static_assert(sizeof(RingBufferHeader) == kChunkSize,
                "The ring control header is part of the experimental ABI");
  static_assert(offsetof(RingBufferHeader, read_pos) == 64,
                "read_pos must not share a cache line with write_pos");
  static_assert(offsetof(RingBufferHeader, reserved) == 128,
                "reserved fields must start on their own cache line");

  // This implementation publishes a write-position reservation before it
  // claims the corresponding physical chunk. The reader can therefore observe
  // a reservation whose writer has not arrived yet; it invalidates and skips
  // that slot rather than waiting, preserving FIFO progress.
  //
  // TODO(sashwinbalaji): revisit this publish-before-claim trade-off before the
  // ring-control layout becomes a stable cross-process ABI. A writer can be
  // descheduled after publishing its position and resume after the physical
  // slot has made a full lap. The post-claim read-position check in the .cc
  // prevents stale data from being published, but the delayed writer can still
  // transiently claim a later incarnation of the slot and create an avoidable
  // hole. Claim-before-publish with helping, or a per-slot sequence number,
  // removes that interference at the cost of more protocol state.
  bool TryReserveWritePos(uint32_t* pos);
  Chunk TryClaimReservedChunk(const ChunkHeader&, uint32_t pos);

  RingBufferHeader* ring_buffer_header() const {
    return reinterpret_cast<RingBufferHeader*>(start_);
  }

  uint8_t* chunk_at(uint32_t pos) const {
    return start_ + sizeof(RingBufferHeader) +
           (pos & chunk_index_mask_) * static_cast<size_t>(kChunkSize);
  }

  static std::atomic<uint32_t>* state_word(uint8_t* chunk) {
    return reinterpret_cast<std::atomic<uint32_t>*>(chunk);
  }

  void AdvanceReadPos();

  base::PagedMemory mem_;
  // Byte-addressed because the allocation contains a control header followed
  // by differently typed fixed-size chunks. Typed objects are constructed once
  // and reached through the accessors above; byte offsets remain the ABI.
  uint8_t* const start_;
  const uint32_t num_chunks_;
  const uint32_t chunk_index_mask_;
  uint32_t read_pos_ = 0;
  Stats stats_;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "The tracing v2 shared state requires lock-free uint32 atomics");
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
              "The tracing v2 shared state requires four-byte atomics");

}  // namespace tracing_v2
}  // namespace perfetto

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_H_
