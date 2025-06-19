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
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/thread_annotations.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/tracing/service/histogram.h"

namespace perfetto {

class TracePacket;
class TraceBufferV2;

namespace internal {

struct TBChunk {
  static constexpr size_t kMaxSize = std::numeric_limits<uint16_t>::max();

  explicit TBChunk(size_t plsz)
      : payload_size(static_cast<uint16_t>(plsz)), exists(1) {
    PERFETTO_DCHECK(plsz <= kMaxSize);
  }

  // NOTE: In the case of scraping we can have two contiguous TBChunks
  // (in the same sequence) with the same chunk_id. One containing all the
  // fragments scraped, the other one the fragments added after scraping, at
  // commit time.
  // TODO think about how to handle this in case we lose the scraped chunk.
  // How do we avoid duped packets (i'm thinking of write_into_file)?
  ChunkID chunk_id = 0;

  // The key to find the SequenceState from TraceBuffer.sequences_.
  ProducerAndWriterID pri_wri_id = 0;

  // Size of the payload, excluding the TBCHunk header itself, and without
  // accounting for any alignment. This doesn't change throughout the lifecycle
  // of a chunk.
  uint16_t payload_size = 0;

  // The size of "unconsumed" payload. This starts at payload_size when a chunk
  // is created (unless it's a zero padding chunk) and goes down to 0 as we
  // read/erase chunks. Payload is consumed always in FIFO order, so the offset
  // of the next unread fragment is (payload_size - payload_avail).
  uint16_t payload_avail = 0;

  // These are == the SharedMemoryABI's chunk flags.
  uint8_t flags = 0;

  // This is always set to 1. It's only 0 on the first pass writing in the ring
  // buffer, when no chunk exists and we hit 0-initialized mmap memory.
  // The fact that a chunk exists doesn't imply that is valid. A padding chunk
  // has exist=1, but pri_wi_id == 0;
  // TODO remove this, not needed after all with used_size_.
  uint8_t exists = 0;

  // Returns the offset to the next unread fragment in the chunk. Note that this
  // points to the next fragment header (the varint with the size) NOT payload.
  uint16_t unread_payload_off() {
    PERFETTO_DCHECK((payload_avail <= payload_size));
    return payload_size - payload_avail;
  }

  static size_t OuterSize(size_t payload) {
    return base::AlignUp<alignof(TBChunk)>(sizeof(TBChunk) + payload);
  }
  size_t outer_size() { return OuterSize(payload_size); }

  bool is_padding() const { return pri_wri_id == 0; }

  uint8_t* fragments_begin() {
    return reinterpret_cast<uint8_t*>(this) + sizeof(TBChunk);
  }

  uint8_t* fragments_end() { return fragments_begin() + payload_size; }
};

// Holds the state for each sequence that has TBCHunk(s) in the buffer.
struct SequenceState {
  SequenceState(ProducerID, WriterID, ClientIdentity);
  ~SequenceState();
  SequenceState(const SequenceState&) noexcept;
  SequenceState& operator=(const SequenceState&) noexcept;

  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  ClientIdentity client_identity{};

  // TODO explain this is like a std::optional<bool> reset on each BeginRead().
  uint64_t skip_in_generation = 0;

  std::optional<ChunkID> last_chunk_id_consumed;

  // An ordered list of chunks (ordered by their chunk_id). Each member
  // corresponsds to the offset within buf_ for the chunk.
  // We store buffer offsets rather than pointers to make buffer cloning easier.
  // In principle this is a queue of TBChunk* (% a call to GetTBChunkAt()).
  base::CircularQueue<size_t> chunks;  // TODO make initial capacity smaller.
};

struct Frag {
  enum FragType : uint8_t {
    kFragWholePacket,
    kFragBegin,
    kFragContinue,
    kFragEnd,
  };

  TBChunk* chunk = nullptr;
  SequenceState* seq = nullptr;  // TODO maybe unneeded delete.

  uint16_t off;      // The offset of the fragment header within the chunk.
  uint16_t size;     // Total size of the fragment, including header.
  uint8_t hdr_size;  // The size of the varint header.
  FragType type = kFragWholePacket;

  uint16_t payload_size() const { return size - hdr_size; }
  uint8_t* begin() { return chunk->fragments_begin() + off + hdr_size; }
  uint8_t* end() { return begin() + size; }
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
  BufIterator();
  // TODO comment on limit.
  explicit BufIterator(TraceBufferV2*, size_t limit = 0);

  static BufIterator CloneReadOnly(const BufIterator&) noexcept;

  void Reset(size_t limit) { *this = BufIterator(buf_, limit); }

  // Depending on the current iteration state, either:
  // 1. Moves next in the linked list, if chunk_ != target_chunk_.
  // 2. Moves next in buffer order, if chunk_ == target_chunk_.
  // If `limit` is non-null, NextChunk returns prematurely false if we hit the
  // `limit` chunk while iterating in buffer order mode. This is used to
  // implement the "DeleteNextChunksFor()" while overwriting.
  bool NextChunk(bool has_erased_current_chunk = false);
  bool NextChunkInSequence(bool has_erased_current_chunk = false);
  bool NextChunkInBuffer(bool first_call_from_ctor = false);
  std::optional<Frag> NextFragmentInChunk();
  void SkipCurrentSequence();
  bool EraseCurrentChunkAndMoveNext();
  void SetChunk(SequenceState* seq, TBChunk* chunk);

  bool valid() {
    PERFETTO_DCHECK((!chunk_ && !target_chunk_) || (chunk_ && target_chunk_));
    return chunk_ != nullptr;
  }
  TBChunk* chunk() { return chunk_; }
  TBChunk* target_chunk() { return target_chunk_; }
  SequenceState* sequence_state() { return seq_; }

 private:
  BufIterator(const BufIterator&) noexcept = default;  // For CopyReadOnly.
  BufIterator& operator=(const BufIterator&) noexcept = default;

  TraceBufferV2* buf_ = nullptr;
  TBChunk* chunk_ = nullptr;
  TBChunk* target_chunk_ = nullptr;

  SequenceState* seq_ = nullptr;

  // This field is the offset within the seq_.chunks list of the current chunk
  // id. This is incremented every time NextChunkInSequence() advances
  // (non-destructively) and reset every time NextChunkInBuffer moves to a
  // different sequence changing `seq_`.
  size_t seq_idx_ = 0;  // TODO rename.

  // Position of the next fragment within the current `chunk_`.
  uint16_t next_frag_off_ = 0;

  bool read_only_iterator_ = false;

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

  // Argument for out-of-band patches applied through TryPatchChunkContents().
  struct Patch {
    // From SharedMemoryABI::kPacketHeaderSize.
    static constexpr size_t kSize = 4;

    size_t offset_untrusted;
    std::array<uint8_t, kSize> data;
  };

  // Can return nullptr if the memory allocation fails.
  static std::unique_ptr<TraceBufferV2> Create(size_t size_in_bytes,
                                               OverwritePolicy = kOverwrite);

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

  bool TryPatchChunkContents(ProducerID,
                             WriterID,
                             ChunkID,
                             const Patch* patches,
                             size_t patches_size,
                             bool other_patches_pending);

  size_t size() const { return size_; }
  size_t used_size() const { return used_size_; }
  OverwritePolicy overwrite_policy() const { return overwrite_policy_; }
  const TraceStats::BufferStats& stats() const { return stats_; }

  void DumpForTesting();

 private:
  using BufIterator = internal::BufIterator;
  using Frag = internal::Frag;
  using SequenceState = internal::SequenceState;

  friend class TraceBufferV2Test;
  friend class internal::BufIterator;

  explicit TraceBufferV2(OverwritePolicy);
  TraceBufferV2(const TraceBufferV2&) = delete;
  TraceBufferV2& operator=(const TraceBufferV2&) = delete;

  // Not using the implicit copy ctor to avoid unintended copies.
  // This tagged ctor should be used only for Clone().
  struct CloneCtor {};
  TraceBufferV2(CloneCtor, const TraceBufferV2&);

  bool Initialize(size_t size);
  TBChunk* CreateTBChunk(size_t off, size_t payload_size);
  bool DeleteNextChunksFor(size_t bytes_to_clear);
  void ConsumeFragment(Frag*);

  enum class FragReassemblyResult { kSuccess = 0, kNotEnoughData, kDataLoss };
  FragReassemblyResult ReassembleFragmentedPacket(TracePacket* out_packet,
                                                  Frag* initial_frag,
                                                  bool force_erase);

  void DcheckIsAlignedAndWithinBounds(size_t off) const {
    PERFETTO_DCHECK((off & (alignof(TBChunk) - 1)) == 0);
    PERFETTO_DCHECK(off <= size_ - sizeof(TBChunk));
  }

  // This should only be used when followed by a placement new.
  // TODO remove this, it's useless.
  TBChunk* GetTBChunkAtUnchecked(size_t off) {
    DcheckIsAlignedAndWithinBounds(off);
    return reinterpret_cast<TBChunk*>(begin() + off);
  }

  TBChunk* GetTBChunkAt(size_t off) {
    TBChunk* tbchunk = GetTBChunkAtUnchecked(off);
    PERFETTO_CHECK(tbchunk->outer_size() <= (size_ - off));
    return tbchunk;
  }

  // Can return nullptr for padding chunks (or in case of programming errors).
  SequenceState* GetSeqForChunk(const TBChunk* chunk) {
    auto it = sequences_.find(chunk->pri_wri_id);
    return it == sequences_.end() ? nullptr : &it->second;
  }

  size_t OffsetOf(const TBChunk* chunk) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(chunk);
    uintptr_t buf_start = reinterpret_cast<uintptr_t>(begin());
    PERFETTO_DCHECK(addr >= buf_start && buf_start <= addr + size_);
    return static_cast<size_t>(addr - buf_start);
  }

  void DiscardWrite() {}  // TODO DNS

  uint8_t* begin() const { return reinterpret_cast<uint8_t*>(data_.Get()); }
  uint8_t* end() const { return begin() + size_; }
  size_t size_to_end() const { return size_ - wr_; }

  base::PagedMemory data_;
  size_t size_ = 0;  // Size in bytes of |data_|.
  size_t wr_ = 0;    // Write cursor (offset since start()).

  // High watermark. The number of bytes (<= |size_|) written into the buffer
  // before the first wraparound. This increases as data is written into the
  // buffer and then saturates at |size_|. Used for CloneReadOnly().
  size_t used_size_ = 0;

  // Statistics about buffer usage.
  TraceStats::BufferStats stats_;

  OverwritePolicy overwrite_policy_ = kOverwrite;

  // Note: we need stable pointers for SequenceState, as they get cached in
  // BufIterator.
  // TODO remember to delete from here if all chunks are gone.
  std::unordered_map<ProducerAndWriterID, SequenceState> sequences_;

  // Iterator used to implement ReadNextTracePacket().
  internal::BufIterator rd_iter_;

  // A generation counter incremented every time BeginRead() is called.
  uint64_t read_generation_ = 0;

  // When true disable some DCHECKs that have been put in place to detect
  // bugs in the producers. This is for tests that feed malicious inputs and
  // hence mimic a buggy producer.
  bool suppress_client_dchecks_for_testing_ = false;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
