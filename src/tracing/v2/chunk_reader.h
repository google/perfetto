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

#ifndef SRC_TRACING_V2_CHUNK_READER_H_
#define SRC_TRACING_V2_CHUNK_READER_H_

#include <stdint.h>

#include <functional>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

// The single consumer of a SharedRingBuffer.
//
// It resolves logical positions in reservation order and never waits for a
// writer. When it lands on a chunk somebody is still writing, it takes the
// prefix that writer has already published and leaves the writer responsible
// for its own unpublished suffix.
//
// Everything the reader reads is treated as untrusted: it copies the committed
// prefix into its own scratch and validates the copy, never the mapping. That
// matters today only as a discipline - producer and consumer are the same
// process in Step 1 - and matters for real once the consumer moves into traced.
//
// Threading: one ChunkReader per ring, used from one execution context.
class ChunkReader {
 public:
  // A view into reader-owned scratch. Valid only for the duration of the
  // Delegate call it was handed to.
  struct Fragment {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
  };

  struct ChunkContents {
    WriterID writer_id = 0;
    BufferID target_buffer = 0;
    // Any of the three PayloadFlags.
    uint32_t payload_flags = 0;
    const Fragment* fragments = nullptr;
    uint32_t num_fragments = 0;
  };

  class Delegate {
   public:
    virtual ~Delegate();
    // Called once per drained chunk that carried usable payload, in the order
    // the positions were reserved.
    virtual void OnChunkRead(const ChunkContents&) = 0;
  };

  // What one call to ReadOne() did: the reader's flowchart has exactly these
  // five exits.
  enum class ReadOutcome {
    // read_pos has caught up with write_pos.
    kNoData,
    // Payload was handed to the delegate and read_pos advanced.
    kEmitted,
    // The position resolved to no payload - nobody claimed it, its writer is
    // mid-rewrite, or the bytes could not be trusted - and read_pos advanced.
    kSkipped,
    // The word kept changing under us for a whole contention budget. read_pos
    // is unchanged and the same position is retried in a later pass.
    kRetryLater,
    // Corruption or an incompatible ABI. This ring is left exactly as it was
    // found and is never read again; the rest of the process is unaffected.
    kProtocolError,
  };

  struct DrainResult {
    uint32_t positions_resolved = 0;
    ReadOutcome last_outcome = ReadOutcome::kNoData;

    // True if the pass stopped with work possibly still queued, which is when
    // the caller has to schedule itself again.
    bool work_may_remain() const {
      return last_outcome != ReadOutcome::kNoData &&
             last_outcome != ReadOutcome::kProtocolError;
    }
  };

  ChunkReader(SharedRingBuffer* ring, Delegate* delegate);
  ~ChunkReader();

  ChunkReader(const ChunkReader&) = delete;
  ChunkReader& operator=(const ChunkReader&) = delete;
  ChunkReader(ChunkReader&&) = delete;
  ChunkReader& operator=(ChunkReader&&) = delete;

  // Resolves at most one position. Does not publish read_pos: that is a
  // per-pass operation, so that one cursor store and at most one wake cover a
  // whole backlog.
  ReadOutcome ReadOne();

  // Resolves up to |max_positions|, then publishes read_pos once and wakes any
  // writer parked on a full ring.
  DrainResult Drain(uint32_t max_positions);

  // Invoked after the committed prefix has been copied and immediately before
  // the compare-and-swap that arbitrates it. It exists so that a test can make
  // the writer win that race deterministically instead of hoping for it.
  // Nothing in production sets it.
  void SetArbitrationHookForTesting(std::function<void()> hook) {
    before_arbitration_hook_for_testing_ = std::move(hook);
  }

  // Starts the reader at |position| instead of zero, to match a ring whose
  // cursors were seeded near the uint32_t rollover.
  void SeekForTesting(uint32_t position) { read_pos_ = position; }

  bool stopped() const { return stopped_; }
  uint32_t read_pos() const { return read_pos_; }

  // Diagnostics. Never inputs to a decision.
  uint64_t num_holes() const { return num_holes_; }
  uint64_t num_scrapes() const { return num_scrapes_; }
  uint64_t num_chunks_emitted() const { return num_chunks_emitted_; }
  uint64_t num_malformed_chunks() const { return num_malformed_chunks_; }
  uint64_t num_unknown_format_chunks() const {
    return num_unknown_format_chunks_;
  }

 private:
  // Copies the committed prefix named by |state_word| into scratch and
  // validates the copy. Returns false if the payload must be dropped - unknown
  // format, or a directory that does not add up. That decides only whether
  // bytes come out; the caller still performs the ownership transition either
  // way, because stopping a stale owner from publishing behind us is a separate
  // question and the only one about safety.
  bool SnapshotCommittedPrefix(uint32_t chunk_index, uint32_t state_word);

  ReadOutcome EmitSnapshot(bool snapshot_usable);
  ReadOutcome Stop(uint32_t state_word);

  SharedRingBuffer* const ring_;
  Delegate* const delegate_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
  const uint32_t chunk_bits_;
  const uint32_t fragment_size_width_;

  // The reader is the only writer of the shared read_pos, so it keeps the
  // authoritative value here and publishes it once per pass. A lagging shared
  // value only under-advertises capacity, which is safe: no ownership decision
  // rests on a cursor.
  uint32_t read_pos_ = 0;
  bool stopped_ = false;
  std::function<void()> before_arbitration_hook_for_testing_;

  // Reader-owned scratch, sized once in the constructor so that draining
  // allocates nothing.
  std::vector<uint8_t> payload_scratch_;
  std::vector<uint8_t> directory_scratch_;
  std::vector<Fragment> fragments_;
  ChunkContents pending_contents_;

  uint64_t num_holes_ = 0;
  uint64_t num_scrapes_ = 0;
  uint64_t num_chunks_emitted_ = 0;
  uint64_t num_malformed_chunks_ = 0;
  uint64_t num_unknown_format_chunks_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_CHUNK_READER_H_
