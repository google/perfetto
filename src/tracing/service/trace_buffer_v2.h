/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_

#include <stdint.h>
#include <string.h>

#include <limits>
#include <unordered_map>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/thread_annotations.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/base/intrusive_list.h"
#include "src/tracing/service/histogram.h"

namespace perfetto {

class TracePacket;
class TraceBufferV2;

namespace internal {
struct TBChunk {
  struct ListTraits {
    static constexpr size_t node_offset() {
      return offsetof(TBChunk, list_node);
    }
  };

  explicit TBChunk(size_t sz) : size(static_cast<uint32_t>(sz)) {
    PERFETTO_DCHECK(sz >= sizeof(TBChunk));
    PERFETTO_DCHECK(sz <= std::numeric_limits<decltype(size)>::max());
  }

  // NOTE: the default-initialization of the fields below matters. It is used
  // by DeleteNextChunksFor() when writing padding chunks.

  // The only case when list_node.prev/next are == nullptr is the case
  // of a padding record, which is not associated to any sequence.
  // TODO replace with two uint32_t offsets.
  base::IntrusiveListNode list_node{};

  // We store the exact (unaligned) size to tell precisely where the last
  // fragment ends. However, TBChunk(s) are stored in the buffer with proper
  // alignment. When we move in the buffer we use aligned_size() not this.
  uint32_t size = 0;  // Size including sizeof(TBChunk).

  // NOTE: In the case of scraping we can have two contiguous TBChunks
  // (in the same sequence) with the same chunk_id. One containing all the
  // fragments scraped, the other one the fragments added after scraping, at
  // commit time.
  // TODO think about how to handle this in case we lose the scraped chunk.
  // How do we avoid duped packets (i'm thinking of write_into_file)?
  ChunkID chunk_id = 0;

  // These are == the SharedMemoryABI's chunk flags.
  uint8_t flags = 0;

  static size_t OuterSize(size_t inner) {
    return base::AlignUp<alignof(TBChunk)>(inner);
  }
  size_t outer_size() { return OuterSize(size); }

  bool is_padding() const { return !list_node.is_attached(); }

  uint8_t* fragments_begin() {
    return reinterpret_cast<uint8_t*>(this) + sizeof(TBChunk);
  }

  uint8_t* fragments_end() { return fragments_begin() + size; }

  // ProducerAndWriterID pr_wr_id = 0;  // TODO maybe not needed
  // TODO do we need chunk_id?
};

using ChunkList = base::IntrusiveList<TBChunk>;

// Holds the state for each sequence that has TBCHunk(s) in the buffer.
struct SequenceState {
  ChunkList chunk_list{};
  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  ClientIdentity client_identity{};
  uint64_t read_pass = 0;
  // ChunkID last_chunk_id = 0;  // TODO is this actually needed? TBD
};

struct Frag {
  enum FragType {
    kFragInvalid = 0,
    kFragWholePacket,
    kFragBegin,
    kFragContinue,
    kFragEnd,
  };
  static constexpr size_t kShift = 1;
  static constexpr uint8_t kMask = (1 << kShift) - 1;
  static constexpr uint8_t kValid = 1 << 0;

  // Points to the varint with (size << kFragShift) & flags.
  uint8_t* size_hdr = nullptr;

  // Points to the payload of the fragment (a few bytes after size_hdr).
  uint8_t* begin = nullptr;

  // The size of the payload (without any flags). This is NOT rounded up to
  // alignof(TBChunk) so we can tell when the last fragment ends.
  // Use outer_size() to determine the boundary of the chunk in the buffer.
  size_t size = 0;

  FragType type = kFragInvalid;

  void MarkInvalid() { *size_hdr &= ~kValid; }
};

// BufIterator encapsulates the logic to move around chunks and fragments in
// the buffer, to implement readback and overwrite logic. There are two ways
// we can iterate chunks in the buffer: (1) following their physical order
// in the ring buffer; (2) following their logical order via the per-sequence
// linked-list. More in details this is how a typical iteration works:
//
// Step 1: identify the "target" chunk we want to read.
// ----------------------------------------------------
// We start the iteration saying "I want to read (or overwrite) this chunk",
// where "this chunk" is the one immediately after the write pointer. That is,
// by definition of ring buffer, the place where the oldest data lives.
// At the time of writing, all iterations (i.e. all BufIterator ctor calls)
// start always at `TraceBuffer.wr_`
//
// Step 2: rewinding back in the sequence using the linked list.
// -------------------------------------------------------------
// We can't just read the target chunk right away. Due to out-of-order commits
// (which are very rare, but possible, due to scraping) there might be chunks
// that logically precedes the current chunk, but are stored physically after
// our target chunk. We want reads to respect FIFO-ness of data, so we
// follow the linked list backwards, until we find the first element of the
// list and start from there (even if that chunk might be physically stored
// after our target chunk address-wise).
//
// Step 3: keep following the linked list until we reach our target chunk.
// -----------------------------------------------------------------------
// In order to respect FIFO-ness, we want to jump around (address-wise)
// wherever needed in the buffer to keep following the chunks in logical
// sequence. Eventually we weill reach back the target chunk we wanted to
// read in the first place. In practice these linked-lsit walks are usually
// rare, and in most cases resolve within a few hops.
//
// Step 4: proceed in buffer order, repeat.
// ----------------------------------------
// Once we have consumed the target chunk, we want to move to the next chunk
// in the buffer in physical (address) order, as we want to consume the ring
// buffer in order. However, moving to the next chunk poses the same problem
// we faced in Step 1 (there might be other chunks that logically precede that
// one), so we have repeat the same walking pattern again.
//
// Overall the iteration in the buffer can be summarized as follows:
// - Set a target chunk using the next chunk in buffer order.
// - Walk all the way back from that target using the linked list.
// - Follow the linked list forward, until we reach back the target.
// - Move to the next chunk in buffer order and repeat.
class BufIterator {
 public:
  // TODO comment on limit.
  explicit BufIterator(TraceBufferV2*, size_t limit = 0);
  BufIterator(const BufIterator&) = default;  // Deliberately copyable.
  BufIterator& operator=(const BufIterator&) = default;

  // Prepare to iterate until the passed target chunk. This starts setting
  // both chunk_ and target_chunk_ to the argument, then walks back chunk_
  // to the beginning of the per-sequence linked list.
  // Returns true if we have already visited the sequence in the current
  // read_pass_, false otherwise.
  bool SetTargetChunkAndRewind(TBChunk*);

  void SetChunk(TBChunk* chunk) {
    chunk_ = chunk;
    next_frag_ = chunk->fragments_begin();
  }

  // Depending on the current iteration state, either:
  // 1. Moves next in the linked list, if chunk_ != target_chunk_.
  // 2. Moves next in buffer order, if chunk_ == target_chunk_.
  // If `limit` is non-null, NextChunk returns prematurely false if we hit the
  // `limit` chunk while iterating in buffer order mode. This is used to
  // implement the "DeleteNextChunksFor()" while overwriting.
  bool NextChunk();

  bool NextChunkInSequence();
  bool NextChunkInBuffer();

  std::optional<Frag> NextFragmentInChunk();

  TBChunk* chunk() { return chunk_; }
  SequenceState* sequence_state() { return seq_; }

 private:
  // These 3 pointers below are never null and always point to a valid portion
  // of the buffer.
  TraceBufferV2* buf_ = nullptr;
  TBChunk* chunk_ = nullptr;
  TBChunk* target_chunk_ = nullptr;

  SequenceState* seq_ = nullptr;

  // TODO add here a target_chunk_ to handle the
  // NextChunk() to tellwhether we did rewind or not.
  // Position of the next fragment within the current `chunk_`.
  uint8_t* next_frag_ = nullptr;

  // TODO explain that limit is only for iterating in buffer order (and why).
  // It stops AFTER passing the limit.
  size_t limit_ = 0;
};
}  // namespace internal

class TraceBufferV2 {
 public:
  using TBChunk = internal::TBChunk;

  // See comment in the header above.
  enum OverwritePolicy { kOverwrite, kDiscard };

  // Identifiers that are constant for a packet sequence.
  struct PacketSequenceProperties {
    ProducerID producer_id_trusted;
    ClientIdentity client_identity_trusted;
    WriterID writer_id;

    uid_t producer_uid_trusted() const { return client_identity_trusted.uid(); }
    pid_t producer_pid_trusted() const { return client_identity_trusted.pid(); }
  };

  void CopyChunkUntrusted(ProducerID producer_id_trusted,
                          const ClientIdentity& client_identity_trusted,
                          WriterID writer_id,
                          ChunkID chunk_id,
                          uint16_t num_fragments,
                          uint8_t chunk_flags,
                          bool chunk_complete,
                          const uint8_t* src,
                          size_t size);

  // To read the contents of the buffer the caller needs to:
  //   BeginRead()
  //   while (ReadNextTracePacket(packet_fragments)) { ... }
  // No other calls to any other method should be interleaved between
  // BeginRead() and ReadNextTracePacket().
  // Reads in the TraceBufferV2 are NOT idempotent.
  void BeginRead(size_t limit = 0);

  bool ReadNextTracePacket(TracePacket*,
                           PacketSequenceProperties* sequence_properties,
                           bool* previous_packet_on_sequence_dropped,
                           bool force_erase = false);

 private:
  using BufIterator = internal::BufIterator;
  using ChunkList = internal::ChunkList;
  using Frag = internal::Frag;
  using SequenceState = internal::SequenceState;

  friend class TraceBufferTest;
  friend class internal::BufIterator;

  explicit TraceBufferV2(OverwritePolicy);
  TraceBufferV2(const TraceBufferV2&) = delete;
  TraceBufferV2& operator=(const TraceBufferV2&) = delete;

  // Not using the implicit copy ctor to avoid unintended copies.
  // This tagged ctor should be used only for Clone().
  struct CloneCtor {};
  TraceBufferV2(CloneCtor, const TraceBufferV2&);

  bool Initialize(size_t size);
  bool DeleteNextChunksFor(size_t bytes_to_clear);
  void EnsureCommitted(size_t);

  using FragSmallVector = base::SmallVector<Frag, 16>;
  enum class FragReassemblyResult { kSuccess = 0, kNotEnoughData, kDataLoss };
  FragReassemblyResult ReassembleFragmentedPacket(FragSmallVector*,
                                                  Frag* initial_frag);

  void DcheckIsAlignedAndWithinBounds(size_t off) const {
    PERFETTO_DCHECK((off & (alignof(TBChunk) - 1)) == 0);
    PERFETTO_DCHECK(off <= size_ - sizeof(TBChunk));
  }

  TBChunk* GetTBChunkAtUnchecked(size_t off) {
    DcheckIsAlignedAndWithinBounds(off);
    // We may be accessing a new (empty) record.
    EnsureCommitted(off + sizeof(TBChunk));
    return reinterpret_cast<TBChunk*>(begin() + off);
  }

  TBChunk* GetTBChunkAt(size_t off) {
    TBChunk* tbchunk = GetTBChunkAtUnchecked(off);
    PERFETTO_CHECK(tbchunk->size >= sizeof(TBChunk) &&
                   tbchunk->size <= (size_ - off));
    return tbchunk;
  }

  size_t OffsetOf(const TBChunk* chunk) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(chunk);
    uintptr_t start = reinterpret_cast<uintptr_t>(begin());
    PERFETTO_DCHECK(start >= addr && start <= addr + size_);
    return static_cast<size_t>(addr - start);
  }

  void EraseTBChunk(TBChunk*);
  void DiscardWrite() {}  // TODO DNS

  uint8_t* begin() const { return reinterpret_cast<uint8_t*>(data_.Get()); }
  // uint8_t* end() const { return begin() + size_; }
  size_t size_to_end() const { return size_ - wr_; }

  base::PagedMemory data_;
  size_t size_ = 0;  // Size in bytes of |data_|.
  size_t wr_ = 0;    // Write cursor (offset since start()).

  // Statistics about buffer usage.
  TraceStats::BufferStats stats_;

  // Note: we need stable pointers for SequenceState, as they get cached in
  // BufIterator.
  std::unordered_map<ProducerAndWriterID, SequenceState> sequences_;

  // Iterator used to implement ReadNextTracePacket().
  internal::BufIterator rd_iter_;

  // A generation counter incremented every time BeginRead() is called.
  uint64_t read_pass_ = 0;

  // When true disable some DCHECKs that have been put in place to detect
  // bugs in the producers. This is for tests that feed malicious inputs and
  // hence mimic a buggy producer.
  bool suppress_client_dchecks_for_testing_ = false;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
