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

#ifndef SRC_TRACING_V2_RING_WRITER_H_
#define SRC_TRACING_V2_RING_WRITER_H_

#include <stdint.h>

#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"

namespace perfetto::tracing_v2 {

// The writer half of the chunk protocol: one instance owns one WriterID's
// chunks: reserve, claim, append, publish, reuse, and the relocation the reader
// forces when it scrapes a chunk out from under the writer.
//
// This class is encoding-agnostic. It hands the caller a contiguous range and
// is told how many bytes were used; it never looks at them.
//
// Threading: one RingWriter is used by one thread at a time, exactly like a v1
// TraceWriter. Several RingWriters, on several threads, share a ring.
//
// The caller must nudge the reader after any call that returns - every one of
// them can move write_pos, and a reservation that produced no payload still has
// to be resolved before the reader can pass it. What that nudge is - a task, an
// eventfd, an IPC - is not this layer's business.
class RingWriter {
 public:
  // How an attempt to obtain payload space ended. The three failure modes look
  // alike in a log and mean different things, so they are counted separately.
  enum class Outcome {
    kOk,
    // The ring is structurally full: num_chunks positions are outstanding and
    // the reader is behind. A stalling policy has already waited by the time
    // this is returned.
    kFull,
    // Positions were reserved but their chunks could not be claimed. Chunks
    // pinned by a stalled writer produce this without the ring being full.
    //
    // It takes precedence over kFull and does not depend on the policy: once a
    // call has burned a reservation, the holes it made are positions only the
    // reader can resolve, so the caller has to nudge the reader and come back.
    // A stalling policy does not wait for them either, because the nudge
    // happens after the call returns.
    kNoChunkAvailable,
    // The request is larger than a freshly claimed chunk could ever hold. This
    // is a caller bug, not backpressure.
    kTooLarge,
    // The reader scraped the chunk and there was no replacement capacity, so
    // the unpublished suffix was dropped. The writer is consistent and its next
    // publication will carry the data-loss flag.
    kRelocationDropped,
  };

  // A contiguous range for the caller to fill. Valid until the next call on
  // this RingWriter.
  struct FragmentSpan {
    Outcome outcome = Outcome::kFull;
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
  };

  RingWriter(SharedRingBuffer* ring,
             WriterID writer_id,
             BufferID target_buffer,
             BufferExhaustedPolicy buffer_exhausted_policy);
  ~RingWriter();

  RingWriter(const RingWriter&) = delete;
  RingWriter& operator=(const RingWriter&) = delete;
  RingWriter(RingWriter&&) = delete;
  RingWriter& operator=(RingWriter&&) = delete;

  // Opens a fragment of at least |min_size| bytes, reusing the cached chunk or
  // obtaining a new one as needed. At most one fragment is open at a time.
  //
  // |continues_from_prev| says that the first bytes written here are the tail
  // of a packet that began in this writer's previous chunk. It is recorded only
  // when this call moves to a new chunk, which is the only case where the flag
  // means anything.
  FragmentSpan OpenFragment(uint32_t min_size, bool continues_from_prev);

  // Closes the open fragment at |size| bytes - which must not exceed the span
  // OpenFragment() handed out - writes its size entry, and publishes.
  //
  // |continues_on_next| says that this fragment is the head of a packet that
  // continues in this writer's next chunk. A chunk published with that flag is
  // never reused, which is what makes every non-empty published prefix end on a
  // packet boundary.
  Outcome CloseFragment(uint32_t size, bool continues_on_next);

  // Publishes whatever is held and lets go of the chunk. Any open fragment is
  // abandoned: its bytes were never counted, so nothing is published for it.
  // Safe to call with nothing held; the destructor calls it.
  Outcome Release();

  // Tells the writer that the caller dropped data, so that the next chunk this
  // writer publishes reports the gap. The writer sets this itself when it has
  // to drop a relocated suffix; callers that drop a packet of their own accord
  // have to say so here.
  void RecordDataLoss() { owes_data_loss_ = true; }

  // Payload bytes the currently held chunk could give to one more fragment.
  // Zero if nothing is held, if the chunk has reached 255 fragments, or if the
  // payload and the directory have met.
  uint32_t AvailableForNextFragment() const;

  bool has_open_fragment() const {
    return open_fragment_begin_ != kNoOpenFragment;
  }
  WriterID writer_id() const { return writer_id_; }
  BufferID target_buffer() const { return target_buffer_; }

  // Diagnostics. Never inputs to a decision.
  uint64_t num_holes() const { return num_holes_; }
  uint64_t num_fragments_dropped() const { return num_fragments_dropped_; }
  uint64_t num_relocations() const { return num_relocations_; }

 private:
  // What this writer currently holds.
  enum class Ownership {
    kNone,
    // We are inside the chunk and may append to it.
    kAcquired,
    // We published it and kept the handle because it can legally take more.
    kCompleteCached,
  };

  static constexpr uint32_t kNoOpenFragment = UINT32_MAX;

  Outcome AcquireChunk(uint32_t carried_flags);
  Outcome PublishHeldChunk(bool continues_on_next);
  FragmentSpan OpenInHeldChunk();
  void DropChunkHandle();

  SharedRingBuffer* const ring_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
  const BufferExhaustedPolicy buffer_exhausted_policy_;
  const uint32_t chunk_size_;
  const uint32_t fragment_size_width_;

  Ownership ownership_ = Ownership::kNone;
  uint8_t* chunk_ = nullptr;
  uint32_t chunk_index_ = 0;
  // The exact word we last successfully wrote into the chunk. Every transition
  // compares against this snapshot; nothing re-reads the chunk to find out what
  // we ourselves put there.
  uint32_t state_word_ = 0;
  // Offset of the end of finalized payload, i.e. the payload cursor.
  uint32_t payload_end_ = 0;
  // Finalized fragments, published or not. The published count lives in
  // state_word_.
  uint32_t num_fragments_ = 0;
  uint32_t open_fragment_begin_ = kNoOpenFragment;
  uint32_t open_fragment_end_ = 0;

  // Set when this writer has lost data that the next chunk it publishes must
  // report.
  bool owes_data_loss_ = false;

  // Sized to one chunk at construction and never grown afterwards, so the
  // relocation path allocates nothing. A relocated suffix is always a subset of
  // one chunk, and the replacement chunk has the same geometry, so one chunk's
  // worth is always enough.
  std::vector<uint8_t> relocation_payload_;
  std::vector<uint32_t> relocation_sizes_;

  uint64_t num_holes_ = 0;
  uint64_t num_fragments_dropped_ = 0;
  uint64_t num_relocations_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_RING_WRITER_H_
