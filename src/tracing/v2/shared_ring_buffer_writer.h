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
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

// Writes the packet fragments produced by one TraceWriter into a
// SharedRingBuffer.
//
// A fragment is one packet, or one part of a packet that crosses a chunk
// boundary. A chunk can hold several fragments. SharedRingBufferWriter deals
// only with their byte ranges and lengths; it does not interpret their bytes.
//
// Each instance is used by one thread at a time. Several instances can write
// to the same ring concurrently.
//
// Reserving a write position and claiming its physical chunk are separate
// operations. If the claim fails, the position is still part of the stream and
// only the reader can resolve it. The writer notifies its delegate before it
// waits for the reader to make space.
class SharedRingBufferWriter {
 public:
  // Result of an operation that may need a new chunk.
  enum class Outcome {
    kOk,
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
    // The reader scraped the chunk and there was no replacement capacity, so
    // the unpublished suffix was dropped. The writer is consistent and its next
    // publication will carry the data-loss flag.
    kRelocationDropped,
  };

  // A contiguous range for the caller to fill. Valid until the next call on
  // this SharedRingBufferWriter.
  struct FragmentSpan {
    Outcome outcome = Outcome::kFull;
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
  };

  class Delegate {
   public:
    virtual ~Delegate();

    // Makes the reader aware of newly reserved positions. Called before a
    // writer applies a buffer-exhaustion policy that may wait for read_pos to
    // advance. Several writers may call this concurrently.
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
  // |continues_from_prev| says that the first bytes written here are the tail
  // of a packet that began in this writer's previous chunk. It is recorded only
  // when this call moves to a new chunk, which is the only case where the flag
  // means anything.
  FragmentSpan OpenFragment(uint32_t min_size, bool continues_from_prev);

  // Closes the open fragment at |size| bytes - which must not exceed the span
  // OpenFragment() handed out - writes its size varint, and publishes.
  //
  // |continues_on_next| says that this fragment is the head of a packet that
  // continues in this writer's next chunk. A chunk published with that flag is
  // never reused, which is what makes every non-empty published prefix end on a
  // packet boundary.
  Outcome CloseFragment(uint32_t size, bool continues_on_next);

  // Publishes whatever is held and lets go of the chunk. Any open fragment is
  // abandoned: its bytes were never counted, so nothing is published for it.
  // Safe to call with nothing held; the destructor calls it.
  Outcome FinishCurrentChunk();

  // Tells the writer that the caller dropped data, so that the next chunk this
  // writer publishes reports the gap. The writer sets this itself when it has
  // to drop a relocated suffix; callers that drop a packet of their own accord
  // have to say so here.
  void RecordDataLoss() { data_loss_pending_ = true; }

  // Payload bytes the current chunk could give to one more fragment.
  // Zero if nothing is held, if the chunk has reached 255 fragments, or if the
  // payload bytes and fragment-size varints have met.
  uint32_t MaxFragmentSizeInCurrentChunk() const;

  bool has_open_fragment() const {
    return open_fragment_begin_ != kNoOpenFragment;
  }
  WriterID writer_id() const { return writer_id_; }
  BufferID target_buffer() const { return target_buffer_; }

  // Diagnostics only.
  uint64_t num_failed_claims() const { return num_failed_claims_; }
  uint64_t num_fragments_dropped() const { return num_fragments_dropped_; }
  uint64_t num_relocations() const { return num_relocations_; }

 private:
  static constexpr uint32_t kNoOpenFragment = UINT32_MAX;

  Outcome AcquireNewChunk(uint32_t carried_flags);
  Outcome CompleteCurrentChunk(bool continues_on_next);
  // Clears only this writer's cached chunk state. It does not modify the ring.
  void ResetCurrentChunk();
  FragmentSpan OpenFragmentInCurrentChunk();

  SharedRingBuffer* const ring_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
  const BufferExhaustedPolicy buffer_exhausted_policy_;
  Delegate* const delegate_;
  const uint32_t chunk_size_;

  // State cached for the chunk this writer currently owns.
  uint8_t* current_chunk_ = nullptr;
  uint32_t current_chunk_index_ = 0;
  // Exact word expected by this writer's next state transition.
  uint32_t expected_state_word_ = 0;
  // Offset of the end of finalized payload.
  uint32_t payload_end_ = 0;
  // Offset of the first fragment-size varint. Sizes are prepended from the end
  // of the chunk towards the payload.
  uint32_t fragment_sizes_begin_ = 0;
  // Finalized fragments, published or not. expected_state_word_ contains the
  // count already visible to the reader.
  uint32_t num_fragments_ = 0;
  uint32_t open_fragment_begin_ = kNoOpenFragment;
  uint32_t open_fragment_end_ = 0;

  // Set when this writer has lost data that the next chunk it publishes must
  // report.
  bool data_loss_pending_ = false;

  // Preallocated so relocation does not allocate.
  std::vector<uint8_t> relocation_payload_;
  std::vector<uint32_t> relocation_fragment_sizes_;

  uint64_t num_failed_claims_ = 0;
  uint64_t num_fragments_dropped_ = 0;
  uint64_t num_relocations_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_WRITER_H_
