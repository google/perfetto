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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_READER_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_READER_H_

#include <stdint.h>

#include <functional>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {

namespace test {
class SharedRingBufferInternalsForTest;
}

// Reads packet fragments from one SharedRingBuffer.
//
// - Use each instance from one execution context. The ring buffer's storage and
//   SharedRingBuffer view must outlive the reader.
// - Reservations are consumed in order. A relocated fragment gets a later
//   reservation.
// - The reader never waits for a writer, but may retry when it loses a
//   concurrent state transition.
// - If a writer still owns a chunk, the reader copies its published fragments.
//   This is a partial chunk read. The writer moves its unpublished fragment to
//   a later reservation.
// - A chunk marked kFlagDataLoss is consumed without delivering its fragments.
//   The delegate receives a loss report instead.
// - Fragment sizes are decoded once into reader-owned values, and the payload
//   copy uses those values without rereading the shared size bytes. The
//   delegate only ever sees reader-owned scratch, never a view of
//   producer-controlled shared memory.
class SharedRingBufferReader {
 public:
  // Result of consuming one position.
  enum class ConsumeResult {
    // No reservation at read_pos is visible yet.
    // read_pos has caught up with the observed write_pos.
    kNoData,
    // A chunk was handed to the delegate and read_pos advanced.
    kChunkRead,
    // No payload was delivered. read_pos advanced and data can no longer be
    // published for this position.
    kPositionSkipped,
    // A writer changed the chunk before the reader's CAS:
    // - Claimed Free before the reader advanced its wrap count.
    // - Published Complete before the reader requested a rewrite.
    // - Reused Complete before the reader reclaimed it.
    // read_pos stays unchanged. Drain() retries the same position immediately,
    // within its per-position attempt limit.
    kRetryImmediately,
    // Invalid ring buffer state. This reader cannot continue.
    // - The offending position is neither advanced nor reclaimed.
    // - Earlier chunks in this drain may already have been delivered.
    // - Drain() still publishes any progress made before the error.
    kProtocolError,
  };

  // A view into reader-owned scratch. Valid only for the duration of the
  // Delegate call.
  struct Fragment {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
  };

  struct ChunkContents {
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    // A bitwise OR of PayloadFlags.
    uint32_t payload_flags = 0;
    const Fragment* fragments = nullptr;
    uint32_t num_fragments = 0;
  };

  struct DrainResult {
    uint32_t positions_consumed = 0;
    ConsumeResult last_result = ConsumeResult::kNoData;

    // The caller should schedule another Drain() call.
    bool needs_another_drain() const {
      return last_result != ConsumeResult::kNoData &&
             last_result != ConsumeResult::kProtocolError;
    }
  };

  // Handles the drained fragments and loss reports. Must outlive the reader.
  // Calls run synchronously from Drain() and must not reenter or destroy the
  // reader.
  class Delegate {
   public:
    virtual ~Delegate();

    // Called after a chunk's published payload has been copied into
    // reader-owned memory. The view is valid only for this call.
    virtual void OnChunkRead(const ChunkContents&) = 0;

    // Called when published data is discarded after validation fails or the
    // chunk carries kFlagDataLoss. Also handles a flagged Complete chunk with
    // no fragments. No OnChunkRead() call delivers those fragments.
    //
    // The delegate must handle packet recovery for this writer:
    // - Discard any partial packet and remember the gap.
    // - A later unflagged chunk may start with an orphan continuation.
    //   Discard that fragment because its earlier bytes were lost.
    // - Later fragments in that chunk can start complete, valid packets.
    //   Deliver those packets and report the gap on the first one.
    virtual void OnDataLoss(WriterID) = 0;
  };

  SharedRingBufferReader(SharedRingBuffer* ring, Delegate* delegate);
  ~SharedRingBufferReader();

  SharedRingBufferReader(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader& operator=(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader(SharedRingBufferReader&&) = delete;
  SharedRingBufferReader& operator=(SharedRingBufferReader&&) = delete;

  // Consumes up to |max_positions| logical reservations:
  // - A position counts even when it delivers no fragments.
  // - An unclaimed reservation uses the budget when the reader passes it.
  // - A lost CAS retries the same position without using the position budget.
  //   A separate attempt limit bounds retries at each position. Reaching it
  //   leaves that position unchanged and requests another drain.
  //
  // Publishes read_pos once and wakes writers parked on a full ring buffer.
  // The budget bounds copying and delegate callbacks in this pass.
  // This lets other tasks run between passes over a large ring buffer.
  DrainResult Drain(uint32_t max_positions);

  bool has_protocol_error() const { return has_protocol_error_; }
  uint32_t read_pos() const { return read_pos_; }

  // For diagnostics only. The protocol never reads these counters.
  // TODO(sashwinbalaji): Wire these counters into service statistics.
  struct Stats {
    uint64_t positions_skipped = 0;
    // Successful BeingWritten -> RewriteRequested transitions.
    uint64_t rewrite_requests = 0;
    uint64_t chunks_read = 0;
    uint64_t malformed_chunks = 0;
    uint64_t unsupported_format_chunks = 0;
  };
  Stats GetStats() const { return stats_; }

 private:
  friend class test::SharedRingBufferInternalsForTest;

  // Consumes at most one position. Drain() publishes read_pos once per pass.
  ConsumeResult ConsumeNextPosition();

  enum class CopiedChunkStatus {
    kNoFragments,
    kReady,
    kMalformed,
    kUnsupportedFormat,
    kDataLoss,
  };

  // Validates and copies the published fragments.
  // Skips payload access if kFlagDataLoss is set. Otherwise validates sizes
  // and format before copying.
  // The caller handles ownership before reporting any data loss.
  CopiedChunkStatus CopyPublishedFragments(ChunkIndex chunk_idx,
                                           uint32_t state_word);

  // Called only after winning the position's compare-and-swap.
  // Delivers valid, unflagged fragments to the delegate. Reports discarded
  // chunks as loss.
  ConsumeResult HandleCopiedChunk(CopiedChunkStatus);

  // Latches the error and logs |reason| once, together with the position and
  // the offending word. read_pos is not advanced.
  ConsumeResult StopOnProtocolError(const char* reason, uint32_t state_word);

  SharedRingBuffer* const ring_;
  Delegate* const delegate_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;

  // The reader owns this value and publishes it once per Drain().
  uint32_t read_pos_ = 0;
  bool has_protocol_error_ = false;

  // Test callback, called just before the reader's chunk-state CAS.
  // - The reader has loaded the state and copied any readable fragments.
  // - The callback makes a writer claim, publish or reuse the chunk.
  //   The reader's CAS then fails because it expects the earlier state.
  // - Tests verify that Drain() retries without advancing read_pos or
  //   delivering fragments from the failed attempt.
  // Empty in production. Tests force the race without scheduling threads.
  std::function<void()> before_state_transition_for_testing_;

  // TODO(sashwinbalaji): Revisit both scratch vectors:
  // - Check heap/stack allocation in production and tests before enlarging
  //   the reader with inline std::array members.
  // - copied_payload_: establish a common maximum chunk size, then use a byte
  //   array of that size.
  // - copied_fragments_: use kMaxFragmentsPerChunk inline Fragment entries.
  // - Measure whether this + offset access improves the read/copy path over
  //   following pointers to separate vector allocations.

  // Published fragment bytes copied out of shared memory.
  std::vector<uint8_t> copied_payload_;

  // Each fragment's decoded size and pointer into copied_payload_.
  std::vector<Fragment> copied_fragments_;

  // Chunk metadata and a view of copied_fragments_ passed to OnChunkRead().
  ChunkContents copied_chunk_;

  Stats stats_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_READER_H_
