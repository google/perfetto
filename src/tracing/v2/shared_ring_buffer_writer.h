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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {

// Writes the packet fragments produced by one TraceWriter into a
// SharedRingBuffer.
//
// - A fragment is one packet, or part of a packet that crosses a chunk
//   boundary. A chunk can hold several fragments. The writer handles their
//   byte ranges and sizes without interpreting their contents.
// - Use each instance from one thread at a time. Several instances can write
//   to the same ring buffer concurrently.
// - Keep the ring buffer's memory and SharedRingBuffer view alive until
//   every writer is destroyed. The destructor still publishes its chunk.
// - Reserving a write position and claiming its physical chunk are separate
//   operations. A failed claim leaves a position that only the reader can
//   consume.
// - The writer notifies its delegate before waiting for the reader to make
//   space.
// - Stalling uses a futex wait when available. Otherwise, it sleeps and retries
//   with the same timeout and buffer-exhaustion policy.
class SharedRingBufferWriter {
 public:
  // Result of BeginFragment(), which may need a new chunk.
  enum class BeginFragmentResult {
    kSuccess,
    // The ring buffer is structurally full: num_chunks positions are
    // outstanding and the reader is behind. A stalling policy has already
    // waited by the time this is returned.
    kFull,
    // Positions were reserved but their chunks could not be claimed. Chunks
    // pinned by a stalled writer produce this without the ring buffer being
    // full.
    //
    // The reader has been notified before this is returned.
    kNoChunkAvailable,
    // The request is larger than a freshly claimed chunk could hold. This is a
    // caller bug, not backpressure.
    kTooLarge,
  };

  // Result of EndFragment() and FinishCurrentChunk(), which publish.
  enum class EndFragmentResult {
    kSuccess,
    // The reader scraped the chunk, but no replacement chunk was available.
    // The unpublished fragment was dropped. The next publication reports loss.
    kRelocationDropped,
  };

  // A contiguous range for the caller to fill. Valid until the next call on
  // this SharedRingBufferWriter.
  struct FragmentRange {
    BeginFragmentResult result = BeginFragmentResult::kFull;
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
  };

  // Schedules reader work when the writer needs space. Must outlive the writer.
  // NotifyReader() can run concurrently on different writers' threads,
  // including during destruction. It must not reenter the calling writer.
  class Delegate {
   public:
    virtual ~Delegate();

    // Schedules reader work after failed claims leave reservations unclaimed
    // and before this writer waits for read_pos to advance.
    virtual void NotifyReader() = 0;
  };

  SharedRingBufferWriter(SharedRingBuffer* ring,
                         WriterID writer_id,
                         BufferID target_buffer,
                         BufferExhaustedPolicy buffer_exhausted_policy,
                         Delegate* delegate);
  ~SharedRingBufferWriter();

  SharedRingBufferWriter(const SharedRingBufferWriter&) = delete;
  SharedRingBufferWriter& operator=(const SharedRingBufferWriter&) = delete;
  SharedRingBufferWriter(SharedRingBufferWriter&&) = delete;
  SharedRingBufferWriter& operator=(SharedRingBufferWriter&&) = delete;

  // Starts a fragment for the caller to fill.
  // - On success, returns a writable range of at least |min_size| bytes.
  // - Reuses space in the current chunk when possible. Otherwise, claims a new
  //   chunk using the configured buffer-exhaustion policy.
  // - Call EndFragment() after writing the bytes.
  // - Only one fragment can be open at a time.
  //
  // |continues_from_prev| describes the packet this fragment belongs to:
  // - true: continues the packet from this writer's previous chunk.
  // - false: starts a new packet.
  // The flag describes the first fragment of a chunk.
  //
  // If BeginFragment(..., true) cannot acquire space, the caller can abandon
  // the rest of the packet. The next packet must start with
  // BeginFragment(..., false), even though the previous chunk still marks a
  // continuation. Only the caller knows that a new packet starts. The writer
  // cannot infer |continues_from_prev| from the previous chunk's flag.
  FragmentRange BeginFragment(uint32_t min_size, bool continues_from_prev);

  // Finishes the fragment with the |size| bytes the caller wrote.
  // - |size| must fit in the range returned by BeginFragment().
  // - Records the size and publishes the fragment, making it readable.
  // - Set |continues_on_next| if the packet needs another chunk.
  //   Leave it false if this fragment ends the packet.
  //   The flag describes the last fragment, so the writer cannot append more
  //   fragments to this chunk after EndFragment(..., true).
  //
  // A packet split across chunks uses matching continuation flags:
  //
  //   chunk A: EndFragment(..., true)   ContinuesOnNext
  //   chunk B: BeginFragment(..., true) ContinuesFromPrev
  //
  // If the reader reaches the chunk before EndFragment() publishes:
  // - It consumes the published fragments and requests a rewrite. If the chunk
  //   carries kFlagDataLoss, it discards those fragments.
  // - It advances past the reservation without access to the open fragment.
  //   It will not return to this reservation for more fragments.
  // - EndFragment() saves the unpublished fragment in private memory and
  //   acknowledges the request. It then tries to publish that fragment in
  //   another chunk.
  //
  // Relocation uses the configured buffer-exhaustion policy and can wait for
  // space. If acquisition fails, EndFragment() returns kRelocationDropped.
  EndFragmentResult EndFragment(uint32_t size, bool continues_on_next);

  // Publishes whatever is held and lets go of the chunk. Any open fragment is
  // abandoned: its bytes were never counted, so nothing is published for it.
  // Safe to call with nothing held. The destructor calls it.
  EndFragmentResult FinishCurrentChunk();

  // Records loss for the next chunk publication.
  // - A cached chunk can still be reused, even when the ring buffer is full.
  // - The publication sets kFlagDataLoss. The reader discards all published
  //   fragments in that chunk. Even complete packets before or after the gap
  //   are discarded. The flag cannot identify which packets are safe.
  // - Data already delivered by the reader cannot be retracted.
  void RecordDataLoss() { data_loss_pending_ = true; }

  WriterID writer_id() const { return writer_id_; }

  // For diagnostics only. The protocol never reads these counters.
  // TODO(sashwinbalaji): Wire these counters into service statistics.
  struct Stats {
    uint64_t failed_claims = 0;
    uint64_t fragments_dropped = 0;
    uint64_t relocations = 0;
  };
  Stats GetStats() const { return stats_; }

 private:
  friend class test::SharedRingBufferInternalsForTest;

  static constexpr uint32_t kNoFragmentOpen = UINT32_MAX;

  // Maximum payload available to one more fragment in the cached chunk, which
  // must exist.
  uint32_t MaxFragmentSizeInCurrentChunk() const;
  ChunkState cur_chunk_state() const {
    return ChunkStateOf(expected_state_word_);
  }
  bool has_open_fragment() const {
    return cur_fragment_begin_ != kNoFragmentOpen;
  }

  FragmentRange BeginFragmentInCurrentChunk(uint32_t available);
  BeginFragmentResult AcquireNewChunk(uint32_t continuation_flags);
  // Only the just-ended fragment can be complete but unpublished:
  // - A size of zero is a valid empty fragment.
  // - nullopt means FinishCurrentChunk() has no fragment to publish.
  EndFragmentResult ReleaseCurrentChunkAsComplete(
      std::optional<uint32_t> fragment_size,
      bool continues_on_next);
  // Clears only this writer's cached chunk state. It does not modify the ring
  // buffer.
  void ResetCurrentChunk();

  SharedRingBuffer* const ring_;
  Delegate* const delegate_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
  const BufferExhaustedPolicy buffer_exhausted_policy_;
  const uint32_t chunk_size_;
  // MaxFragmentSizeForEmptyChunk(chunk_size_), computed once.
  const uint32_t max_fragment_size_;

  // State cached for the chunk this writer currently owns.
  uint8_t* cur_chunk_ = nullptr;
  ChunkIndex cur_chunk_idx_ = ChunkIndex::FromIndex(0);
  // The exact word this writer's next compare-and-swap expects. It can differ
  // from the shared word once the reader has requested a rewrite.
  uint32_t expected_state_word_ = 0;
  // Payload grows from the header. Size entries grow backwards from the end.
  // - payload_end_ is measured from the chunk start.
  // - size_directory_bytes_ counts bytes used from the chunk end.
  //
  // Example: a 256-byte chunk with 200 payload bytes and 4 directory bytes.
  //
  //   0        6                     206                252         256
  //   +--------+---------------------+------------------+-----------+
  //   | header |  payload grows ---> |    free space    | <-- sizes |
  //   +--------+---------------------+------------------+-----------+
  //
  // Here: chunk_size_ = 256, payload_end_ = 206, size_directory_bytes_ = 4.
  // Directory start = chunk_size_ - size_directory_bytes_ = 252.
  // Free bytes = chunk_size_ - payload_end_ - size_directory_bytes_ = 46.
  //
  // Byte offset from the chunk start to the end of finalized payload.
  uint32_t payload_end_ = 0;
  // Size-directory bytes used from the chunk end. Zero means no entries.
  uint32_t size_directory_bytes_ = 0;
  // Finalized fragments, published or not. expected_state_word_ contains the
  // count already visible to the reader.
  uint32_t num_fragments_ = 0;
  // Byte offset from the chunk start to the open fragment, or kNoFragmentOpen.
  uint32_t cur_fragment_begin_ = kNoFragmentOpen;

  // Whether kFlagDataLoss still needs to be set in a chunk's state word.
  // - Cleared after successfully claiming or publishing a chunk with the flag.
  // - Kept pending if the reader requests a rewrite before publication.
  //   AcquireNewChunk() then sets the flag in the replacement chunk.
  bool data_loss_pending_ = false;

  // Stop trying the syscall if the build or kernel cannot provide it.
  // Once false, this writer uses sleep backoff for later waits too.
  bool use_futex_ = SharedRingBuffer::SupportsWriterWait();

  // Unpublished fragment saved while changing chunks. Allocated on demand and
  // reused. Writers that never relocate need no payload storage here.
  std::vector<uint8_t> relocation_payload_;

  Stats stats_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_
