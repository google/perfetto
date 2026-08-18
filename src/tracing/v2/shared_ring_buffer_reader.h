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
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

// Reads packet fragments from one SharedRingBuffer.
//
// Each instance is used from one execution context. It resolves write
// positions in order and never waits for a writer. If a writer still owns the
// chunk, the reader takes its published prefix and leaves the writer to move
// the unpublished suffix.
//
// Initially the reader runs in the producer process. It validates the fragment
// directory in place and copies the published payload before calling the
// delegate.
//
// TODO(sashwinbalaji): before moving SharedRingBufferReader into traced, copy
// the published format header, directory and payload into reader-owned memory
// before parsing them. A producer must not be able to change service-side
// validation inputs.
class SharedRingBufferReader {
 public:
  // Result of resolving one position.
  enum class Outcome {
    // read_pos has caught up with write_pos.
    kNoData,
    // A chunk was handed to the delegate and read_pos advanced.
    kChunkRead,
    // The position resolved to no payload - nobody claimed it, its writer is
    // mid-rewrite, or the bytes could not be trusted - and read_pos advanced.
    kSkipped,
    // The chunk state changed before the reader completed its transition.
    // read_pos is unchanged and the same position is retried later.
    kRetryLater,
    // Corruption or an incompatible ABI. This ring is left exactly as it was
    // found and is never read again; the rest of the process is unaffected.
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
    // Any of the three PayloadFlags.
    uint32_t payload_flags = 0;
    const Fragment* fragments = nullptr;
    uint32_t num_fragments = 0;
  };

  struct DrainResult {
    uint32_t positions_resolved = 0;
    Outcome last_outcome = Outcome::kNoData;

    // The caller should schedule another Drain() call.
    bool needs_another_drain() const {
      return last_outcome != Outcome::kNoData &&
             last_outcome != Outcome::kProtocolError;
    }
  };

  class Delegate {
   public:
    virtual ~Delegate();

    // Called after a chunk's published payload has been copied into
    // reader-owned memory. The view is valid only for this call.
    virtual void OnChunkRead(const ChunkContents&) = 0;

    // The reader resolved committed data that it could not forward. The
    // consumer should report the gap on the next packet from this writer.
    virtual void OnDataLoss(WriterID) = 0;
  };

  SharedRingBufferReader(SharedRingBuffer* ring, Delegate* delegate);
  ~SharedRingBufferReader();

  SharedRingBufferReader(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader& operator=(const SharedRingBufferReader&) = delete;
  SharedRingBufferReader(SharedRingBufferReader&&) = delete;
  SharedRingBufferReader& operator=(SharedRingBufferReader&&) = delete;

  // Resolves up to |max_positions|, then publishes read_pos once and wakes any
  // writer parked on a full ring.
  DrainResult Drain(uint32_t max_positions);

  // Resolves at most one position. Drain() publishes read_pos once per pass.
  Outcome ResolveNextPosition();

  bool has_protocol_error() const { return has_protocol_error_; }
  uint32_t read_pos() const { return read_pos_; }

  // Diagnostics. Never inputs to a decision.
  uint64_t num_positions_skipped() const { return num_positions_skipped_; }
  uint64_t num_scrapes() const { return num_scrapes_; }
  uint64_t num_chunks_read() const { return num_chunks_read_; }
  uint64_t num_malformed_chunks() const { return num_malformed_chunks_; }
  uint64_t num_unknown_format_chunks() const {
    return num_unknown_format_chunks_;
  }

  // Invoked after the committed prefix has been copied and immediately before
  // the compare-and-swap that arbitrates it. It exists so that a test can make
  // the writer win that race deterministically instead of hoping for it.
  // Nothing in production sets it.
  void SetArbitrationHookForTesting(std::function<void()> hook) {
    before_arbitration_hook_for_testing_ = std::move(hook);
  }

  // Starts the reader at |position| instead of zero, to match a ring seeded
  // near uint32_t rollover.
  void SetReadPosForTesting(uint32_t position) { read_pos_ = position; }

 private:
  enum class CommittedPrefixStatus {
    kNoFragments,
    kReady,
    kMalformed,
    kUnsupportedFormat,
  };

  // Validates and copies the committed prefix. A malformed or unknown format
  // is dropped without changing the ownership transition chosen by the
  // caller.
  CommittedPrefixStatus CopyCommittedPrefix(uint32_t chunk_index,
                                            uint32_t state_word);

  Outcome ForwardCopiedChunk(CommittedPrefixStatus);
  Outcome StopOnProtocolError(uint32_t state_word);

  SharedRingBuffer* const ring_;
  Delegate* const delegate_;
  const uint32_t num_chunks_;
  const uint32_t chunk_size_;
  const uint32_t chunk_index_bits_;

  // The reader owns this value and publishes it once per Drain().
  uint32_t read_pos_ = 0;
  bool has_protocol_error_ = false;
  std::function<void()> before_arbitration_hook_for_testing_;

  // Reader-owned scratch, sized once in the constructor so that draining
  // allocates nothing.
  std::vector<uint8_t> copied_payload_;
  std::vector<Fragment> copied_fragments_;
  ChunkContents copied_chunk_;

  uint64_t num_positions_skipped_ = 0;
  uint64_t num_scrapes_ = 0;
  uint64_t num_chunks_read_ = 0;
  uint64_t num_malformed_chunks_ = 0;
  uint64_t num_unknown_format_chunks_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_READER_H_
