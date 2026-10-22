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

#include <functional>
#include <optional>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
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
// - The writer gets a chunk in two steps:
//   1. Reserve a write position. This moves write_pos.
//   2. Claim the chunk at that position: change its state from Free to
//      BeingWritten.
//   Step 2 can fail, for example if an older writer still holds the chunk.
//   The reserved position then stays unused, and only the reader can move
//   past it.
// - The writer only accesses the ring buffer. It does not contact the reader,
//   and it does not apply a BufferExhaustedPolicy. TraceWriterV2Impl does
//   both.
// - A call that needs a new chunk makes at most num_chunks attempts to
//   reserve and claim one. If none succeeds, the call returns at once with
//   kFull or kClaimFailed. Only WaitForReadPosChange() blocks.
//
// The writer is in one of three states:
// - Ready: no fragment is open. The writer can still hold the chunk that it
//   published last. It appends the next fragment there if the fragment fits,
//   without a new reservation.
// - Fragment open: BeginFragment() returned a range. The caller writes the
//   fragment's bytes into it.
// - Relocation pending: the reader took the chunk before EndFragment()
//   published the fragment, and the writer could not claim a replacement
//   chunk. The writer keeps a copy of the fragment. See EndFragment().
//
// The calls and the states that allow them:
// - BeginFragment(): Ready. On success, opens a fragment.
// - EndFragment(): Fragment open. Publishes the fragment and returns to
//   Ready, or enters Relocation pending.
// - RetryRelocation(): Relocation pending. Publishes the saved copy and
//   returns to Ready, or stays in Relocation pending.
// - DropRelocation(): Relocation pending. Drops the saved copy and returns to
//   Ready.
// - FinishCurrentChunk(): Ready or Fragment open. Releases the held chunk, so
//   the next fragment needs a new one. TraceWriterV2Impl::Flush() uses it.
//   - In Fragment open, it abandons the open fragment and publishes the
//     earlier fragments of the chunk.
// - WaitForReadPosChange(): after a call returned kFull or kClaimFailed.
// - RecordDataLoss(), has_pending_data_loss(), accessors and GetStats(): any
//   state.
class SharedRingBufferWriter {
 public:
  // Result of BeginFragment(), which may need a new chunk.
  enum class BeginFragmentResult {
    kSuccess,
    // The ring buffer is structurally full: num_chunks positions are
    // outstanding and the reader is behind. The call reserved nothing.
    kFull,
    // The call reserved a position, but could not claim its chunk, for example
    // because an older writer still holds it.
    //
    // The reserved position stays unused until the reader moves past it.
    // Chunks pinned by a stalled writer produce this without the ring buffer
    // being full.
    kClaimFailed,
    // The request is larger than a freshly claimed chunk could hold. This is a
    // caller bug, not backpressure.
    kTooLarge,
  };

  // Result of EndFragment(), RetryRelocation() and FinishCurrentChunk(), which
  // publish.
  enum class EndFragmentResult {
    kSuccess,
    // The reader took the chunk before the publication, and the writer could
    // not claim a replacement chunk. The reasons are the same as in
    // BeginFragmentResult. The writer is then in Relocation pending.
    kFull,
    kClaimFailed,
  };

  // Result of RetryRelocation().
  struct RetryRelocationResult {
    EndFragmentResult result = EndFragmentResult::kFull;
    // True if the call claimed at least one replacement chunk.
    // - A successful claim ends one acquisition. A caller that limits each
    //   acquisition, such as TraceWriterV2Impl, then starts a new limit.
    // - With kFull or kClaimFailed, the reader also took the last replacement
    //   before its publication, and the next claim attempts failed.
    bool acquired_replacement = false;
  };

  // A contiguous range for the caller to fill. Valid until the next call on
  // this SharedRingBufferWriter.
  struct FragmentRange {
    BeginFragmentResult result = BeginFragmentResult::kFull;
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
  };

  SharedRingBufferWriter(SharedRingBuffer* ring,
                         WriterID writer_id,
                         BufferID target_buffer);
  ~SharedRingBufferWriter();

  SharedRingBufferWriter(const SharedRingBufferWriter&) = delete;
  SharedRingBufferWriter& operator=(const SharedRingBufferWriter&) = delete;
  SharedRingBufferWriter(SharedRingBufferWriter&&) = delete;
  SharedRingBufferWriter& operator=(SharedRingBufferWriter&&) = delete;

  // Starts a fragment for the caller to fill.
  // - On success, returns a writable range of at least |min_size| bytes.
  // - Reuses space in the current chunk when possible. Otherwise, tries to
  //   claim a new chunk. See the class comment.
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

  // Ends the open fragment at |size| bytes and publishes it, so the reader
  // can copy it. |size| must fit the range from BeginFragment().
  //
  // Set |continues_on_next| if the packet continues in the next chunk, which
  // then starts with BeginFragment(..., true). The flag marks the chunk's last
  // fragment, so the writer appends nothing more to this chunk.
  //
  // The reader can take a chunk while its last fragment is still open: it
  // copies the published fragments and asks the writer to move the open one.
  // EndFragment() then relocates the fragment:
  // 1. It saves a copy and acknowledges the request, so the reader can reuse
  //    the old chunk.
  // 2. It claims a new chunk, copies the fragment there and publishes it. If
  //    the reader takes this chunk too, it repeats.
  // If step 2 cannot claim a chunk, returns kFull or kClaimFailed. The
  // relocation is then pending: call RetryRelocation() or DropRelocation().
  EndFragmentResult EndFragment(uint32_t size, bool continues_on_next);

  // Tries again to claim a chunk for the saved fragment, and publishes it
  // there, as EndFragment() does. Only in Relocation pending.
  RetryRelocationResult RetryRelocation();

  // Drops the saved fragment. The next publication sets kFlagDataLoss. Only in
  // Relocation pending.
  void DropRelocation();

  // Blocks the calling thread. The wait requests at most |timeout_ms|.
  // Scheduling can make the elapsed time longer.
  //
  // Call it after kFull or kClaimFailed. A return does not reserve space. Call
  // again to try for a chunk.
  // - With a futex: returns when read_pos differs from the value that the
  //   last reservation attempt saw, at the timeout, or on a spurious wake. So
  //   if the reader moved read_pos after that attempt, it returns at once.
  // - Without a futex: sleeps for the next step of v1's backoff, 0, 8, 72, ...
  //   microseconds, at most 100 ms per step. No sleep requests more than
  //   |timeout_ms|. It does not check read_pos.
  void WaitForReadPosChange(uint32_t timeout_ms);

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

  // True until a claimed or published chunk carries the recorded loss. This
  // includes a loss flag that a pending relocation inherited from the
  // rewritten chunk.
  //
  // TraceWriterV2Impl uses it so that kStallThenDrop does not stall again for
  // each dropped packet.
  bool has_pending_data_loss() const {
    return data_loss_pending_ ||
           (pending_relocation_ &&
            (pending_relocation_->chunk_flags & kFlagDataLoss));
  }

  WriterID writer_id() const { return writer_id_; }
  // Largest range that BeginFragment() can return.
  uint32_t max_fragment_size() const { return max_fragment_size_; }

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

  // A saved fragment that waits for a replacement chunk. Its bytes are in
  // |relocation_payload_|.
  struct PendingRelocation {
    // Flags for the replacement chunk's BeingWritten word. Only
    // kFlagContinuesFromPrevChunk and kFlagDataLoss, inherited from the
    // rewritten chunk.
    uint32_t chunk_flags = 0;
    // Sets kFlagContinuesOnNextChunk on the replacement's publication.
    bool continues_on_next = false;
  };

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
  // Writes the saved fragment into the newly claimed chunk, as its only
  // fragment.
  void CopyRelocatedFragment(uint32_t fragment_size);
  // Clears only this writer's cached chunk state. It does not modify the ring
  // buffer.
  void ResetCurrentChunk();

  SharedRingBuffer* const ring_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
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
  // - A rewrite of BeingWritten(0) can move the chunk's loss flag into
  //   |pending_relocation_|, not back into this flag. has_pending_data_loss()
  //   checks both.
  bool data_loss_pending_ = false;

  // The read_pos that the last reservation attempt saw.
  // WaitForReadPosChange() waits while read_pos keeps this value.
  uint32_t read_pos_for_wait_ = 0;

  // Stop trying the syscall if the build or kernel cannot provide it.
  // Once false, this writer uses sleep backoff for later waits too.
  bool use_futex_ = SharedRingBuffer::SupportsWriterWait();

  // The next sleep of the backoff without futex. It grows with each sleep, up
  // to 100 ms, across calls. It resets when the writer claims a chunk.
  uint32_t fallback_sleep_us_ = 0;

  // Set when a relocation cannot claim a replacement chunk. RetryRelocation()
  // clears it after a claim. DropRelocation() also clears it.
  std::optional<PendingRelocation> pending_relocation_;

  // Unpublished fragment saved while changing chunks. Allocated on demand and
  // reused. Writers that never relocate need no payload storage here.
  std::vector<uint8_t> relocation_payload_;

  // Test callback, called after each replacement claim, before the
  // publication.
  // - The callback requests a rewrite of the replacement, as the reader can.
  //   The publication then fails, and the writer relocates again.
  // Empty in production. Tests force the race without scheduling threads.
  std::function<void()> after_replacement_claim_for_testing_;

  Stats stats_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_
