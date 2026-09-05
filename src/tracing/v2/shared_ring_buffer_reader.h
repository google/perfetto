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
// Each instance is used from one execution context. It resolves write
// positions in order and never waits for a writer. If a writer still owns the
// chunk, the reader takes its published prefix and leaves the writer to move
// the unpublished suffix. The ring's memory and the SharedRingBuffer view must
// outlive the reader.
//
// Initially the reader runs in the producer process. It validates the fragment
// sizes in place and copies the published payload before calling the delegate.
class SharedRingBufferReader {
 public:
  // Result of resolving one position.
  enum class ResolveResult {
    // read_pos has caught up with write_pos.
    kNoData,
    // A chunk was handed to the delegate and read_pos advanced.
    kChunkRead,
    // No payload was delivered. read_pos advanced and data can no longer be
    // published for this position.
    kPositionSkipped,
    // The chunk state changed before the reader completed its transition.
    // read_pos is unchanged and the same position is retried later.
    kRetryLater,
    // Corruption or an incompatible ABI. This ring is left exactly as it was
    // found and is never read again. The rest of the process is unaffected.
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
    uint32_t positions_resolved = 0;
    ResolveResult last_result = ResolveResult::kNoData;

    // The caller should schedule another Drain() call.
    bool needs_another_drain() const {
      return last_result != ResolveResult::kNoData &&
             last_result != ResolveResult::kProtocolError;
    }
  };

  class Delegate {
   public:
    virtual ~Delegate();

    // Called after a chunk's published payload has been copied into
    // reader-owned memory. The view is valid only for this call.
    virtual void OnChunkRead(const ChunkContents&) = 0;

    // Called for a loss that no OnChunkRead() call can carry: committed data
    // that failed validation, or kFlagDataLoss on a Complete chunk with no
    // fragments. The consumer should report the gap on the next packet from
    // this writer.
    virtual void OnDataLoss(WriterID) = 0;
  };

  SharedRingBufferReader(SharedRingBuffer* ring, Delegate* delegate);
  ~SharedRingBufferReader();

  SharedRingBufferReader(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader& operator=(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader(SharedRingBufferReader&&) = delete;
  SharedRingBufferReader& operator=(SharedRingBufferReader&&) = delete;

  // Resolves up to |max_positions|, then publishes read_pos once and wakes any
  // writer parked on a full ring. Without the bound, one pass over a large
  // ring could monopolize the consumer's task sequence. The bound also caps
  // the copying and delegate work done per task and decides how often
  // read_pos gets published.
  DrainResult Drain(uint32_t max_positions);

  bool has_protocol_error() const { return has_protocol_error_; }
  uint32_t read_pos() const { return read_pos_; }

  // For diagnostics only. The protocol never reads these counters.
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

  // Resolves at most one position. Drain() publishes read_pos once per pass.
  ResolveResult ResolveNextPosition();

  enum class CommittedPrefixStatus {
    kNoFragments,
    kReady,
    kMalformed,
    kUnsupportedFormat,
  };

  // Validates and copies the committed prefix. A malformed or unknown format
  // is dropped without changing the ownership transition chosen by the
  // caller.
  CommittedPrefixStatus CopyCommittedPrefix(uint32_t chunk_idx,
                                            uint32_t state_word);

  // Delivers a valid prefix to the delegate. Invalid or unsupported data is
  // reported as data loss. Called only after the position's compare-and-swap
  // won.
  ResolveResult HandleCommittedPrefix(CommittedPrefixStatus);

  // Latches the error and logs it once. read_pos is not advanced.
  ResolveResult StopOnProtocolError(uint32_t state_word);

  SharedRingBuffer* const ring_;
  Delegate* const delegate_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;

  // The reader owns this value and publishes it once per Drain().
  uint32_t read_pos_ = 0;
  bool has_protocol_error_ = false;

  // Reader-owned scratch, sized once in the constructor so that draining
  // allocates nothing.
  std::vector<uint8_t> copied_payload_;
  std::vector<Fragment> copied_fragments_;
  ChunkContents copied_chunk_;

  Stats stats_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_READER_H_
