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

#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {

// Writes the packet fragments produced by one TraceWriter into a
// SharedRingBuffer.
//
// A fragment is one packet, or one part of a packet that crosses a chunk
// boundary. A chunk can hold several fragments. SharedRingBufferWriter deals
// only with their byte ranges and sizes; it does not interpret their bytes.
//
// Each instance is used by one thread at a time. Several instances can write
// to the same ring concurrently. The ring's memory and the SharedRingBuffer
// view must outlive every writer: the destructor still publishes the chunk the
// writer holds.
//
// Reserving a write position and claiming its physical chunk are separate
// operations. If the claim fails, the position is still part of the stream and
// only the reader can resolve it. The writer notifies its delegate before it
// waits for the reader to make space.
class SharedRingBufferWriter {
 public:
  // Result of BeginFragment(), which may need a new chunk.
  enum class BeginFragmentResult {
    kSuccess,
    // The ring is structurally full: num_chunks positions are outstanding and
    // the reader is behind. A stalling policy has already waited by the time
    // this is returned.
    kFull,
    // Positions were reserved but their chunks could not be claimed. Chunks
    // pinned by a stalled writer produce this without the ring being full.
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
    // The reader scraped the chunk and there was no replacement capacity, so
    // the unpublished suffix was dropped. The writer is consistent and its next
    // publication will carry the data-loss flag.
    kRelocationDropped,
  };

  // A contiguous range for the caller to fill. Valid until the next call on
  // this SharedRingBufferWriter.
  struct FragmentRange {
    BeginFragmentResult result = BeginFragmentResult::kFull;
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
  };

  class Delegate {
   public:
    virtual ~Delegate();

    // Schedules reader work. Called after this writer creates holes and before
    // it waits for read_pos to advance. May be called concurrently by several
    // writers.
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

  // Returns space for one fragment of at least |min_size| bytes. This reuses
  // the current chunk when possible and acquires a new chunk otherwise. At
  // most one fragment can be open at a time.
  //
  // A packet split across chunks publishes matching continuation flags:
  //
  //   chunk A: EndFragment(..., true)   ContinuesOnNext
  //   chunk B: BeginFragment(..., true) ContinuesFromPrev
  //
  // The caller supplies both because a failed continuation can enter the drop
  // buffer, making the next fragment a new packet.
  FragmentRange BeginFragment(uint32_t min_size, bool continues_from_prev);

  // Ends the open fragment at |size| bytes, writes its size varint, and
  // publishes. |size| must not exceed the range BeginFragment() handed out. If
  // the reader scraped the chunk meanwhile, the unpublished suffix moves to a
  // new chunk, which applies the buffer-exhaustion policy and may wait.
  //
  // |continues_on_next| says that this fragment is the head of a packet that
  // continues in this writer's next chunk. A chunk published with that flag is
  // never reused, so every non-empty published prefix ends on a packet
  // boundary.
  EndFragmentResult EndFragment(uint32_t size, bool continues_on_next);

  // Publishes whatever is held and lets go of the chunk. Any open fragment is
  // abandoned: its bytes were never counted, so nothing is published for it.
  // Safe to call with nothing held; the destructor calls it.
  EndFragmentResult FinishCurrentChunk();

  // Marks data discarded by the caller. The next chunk published by this writer
  // carries kFlagDataLoss.
  void RecordDataLoss() { data_loss_pending_ = true; }

  WriterID writer_id() const { return writer_id_; }

  // For diagnostics only. The protocol never reads these counters.
  struct Stats {
    uint64_t failed_claims = 0;
    uint64_t fragments_dropped = 0;
    uint64_t relocations = 0;
  };
  Stats GetStats() const { return stats_; }

 private:
  static constexpr uint32_t kNoFragmentOpen = UINT32_MAX;

  // Maximum payload available to one more fragment in the cached chunk.
  uint32_t MaxFragmentSizeInCurrentChunk() const;
  bool has_open_fragment() const {
    return cur_fragment_begin_ != kNoFragmentOpen;
  }

  // |available| is MaxFragmentSizeInCurrentChunk(), computed by the caller.
  FragmentRange BeginFragmentInCurrentChunk(uint32_t available);
  BeginFragmentResult AcquireNewChunk(uint32_t flags);
  EndFragmentResult CompleteCurrentChunk(bool continues_on_next);
  // Clears only this writer's cached chunk state. It does not modify the ring.
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
  uint32_t cur_chunk_idx_ = 0;
  // The exact word this writer's next compare-and-swap expects. It can differ
  // from the shared word once the reader has requested a rewrite.
  uint32_t expected_state_word_ = 0;
  // Payload grows up from the chunk header and the size varints grow down
  // from the end of the chunk. The two offsets below are the edges that move:
  //
  //   0     6                payload_end_    sizes_begin_       chunk_size
  //   +-----+-----------------+------------------+------------------+
  //   | hdr | payload  -----> |       free       | <-----   sizes   |
  //   +-----+-----------------+------------------+------------------+
  //
  // Offset of the end of finalized payload.
  uint32_t payload_end_ = 0;
  // Offset of the lowest size varint written so far.
  uint32_t sizes_begin_ = 0;
  // Finalized fragments, published or not. expected_state_word_ contains the
  // count already visible to the reader.
  uint32_t num_fragments_ = 0;
  // Payload offset of the open fragment, or kNoFragmentOpen.
  uint32_t cur_fragment_begin_ = kNoFragmentOpen;

  // Set when this writer has lost data that the next chunk it publishes must
  // report.
  bool data_loss_pending_ = false;

  // Reserved up front. A scrape can hit at any time, and the rewrite path is
  // not the place to find out that we need to allocate.
  //
  // TODO(sashwinbalaji): Each writer reserves payload storage plus a 255-entry
  // size vector for rewrites. Measure this cost.
  std::vector<uint8_t> relocation_payload_;
  std::vector<uint32_t> relocation_fragment_sizes_;

  Stats stats_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_
