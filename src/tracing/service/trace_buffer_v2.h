/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include <optional>
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
#include "src/tracing/service/trace_buffer.h"

namespace perfetto {

class TracePacket;
class TraceBufferV2;

namespace internal {

// A TraceBuffer Chunk. This is very similar to a Shmem ABI Chunk with the
// following exceptions:
// - The size of the chunk is variable (% 16B alignment) and matches the payload
//   to minimize internal fragmentation and buffer efficiency.
// - The metadata (ChunkID, etc) is stored slighly differently
// - It keeps track both of the size of the original payload, and the consumed
//   reads in the buffer are destructive.
struct TBChunk {
  static constexpr size_t kMaxSize = std::numeric_limits<uint16_t>::max();
  static uint8_t Checksum(size_t off, size_t size) {
    // Note: the checksum must be 0 for (off=0,size=0). See the comment in
    // ReadNextTracePacket() about the edge case of the buffer completely empty.
    return ((off >> 24) ^ (off >> 16) ^ (off >> 8) ^ off ^ (size >> 8) ^ size) &
           0xFF;
  }

  explicit TBChunk(size_t off, size_t size_)
      : size(static_cast<uint16_t>(size_)), checksum(Checksum(off, size)) {
    PERFETTO_DCHECK(size_ <= kMaxSize);
  }

  // The ChunkID, as specified by the TraceWriter in the original SMB chunk.
  ChunkID chunk_id = 0;

  // A combination of producer and writer ID. This forms the primary key to
  // look up the corresponding SequenceState from TraceBuffer.sequences_.
  ProducerAndWriterID pri_wri_id = 0;

  // Size of the chunk, excluding the TBCHunk header itself, and without
  // accounting for any alignment. This doesn't change throughout the lifecycle
  // of a chunk.
  uint16_t size = 0;

  // The size of the valid fragments payload. This is typically == size, with
  // the exception of incomplete chunks committed while scraping.
  // The payload of incomplete chunks can increase (up to the original chunk
  // size). Wheh we scrape we set size = SMB chunk size, and
  // payload_size = all_frag_size.
  uint16_t payload_size = 0;

  // The number of payload bytes unconsumed. This starts at payload_size and
  // shrinks until it reaches 0 as we consume fragments.
  // It is always <= size and <= payload_size.
  // Effectively (payload_size - payload-avail) is the offset of the the next
  // unconsumed fragment header (the varint with the size).
  uint16_t payload_avail = 0;

  // These are == the SharedMemoryABI's chunk flags, with the addition of
  // kChunkComplete (0x80) which doesn't exist at the ABI level, but only here.
  uint8_t flags = 0;

  // This is used only for DCHECKS to verify the integrity of the chunk.
  // This is a hash of the offset in the buffer and the size.
  uint8_t checksum = 0;

  // Returns the offset to the next unread fragment in the chunk. Note that this
  // points to the next fragment header (the varint with the size) NOT payload.
  uint16_t unread_payload_off() {
    PERFETTO_DCHECK((payload_avail <= payload_size));
    return payload_size - payload_avail;
  }

  static inline size_t OuterSize(size_t sz) {
    return base::AlignUp<alignof(TBChunk)>(sizeof(TBChunk) + sz);
  }
  size_t outer_size() { return OuterSize(size); }

  bool is_padding() const { return pri_wri_id == 0; }

  uint8_t* fragments_begin() {
    return reinterpret_cast<uint8_t*>(this) + sizeof(TBChunk);
  }

  uint8_t* fragments_end() { return fragments_begin() + payload_size; }

  bool IsChecksumValid(size_t off) { return Checksum(off, size) == checksum; }
};

// Holds the state for each sequence that has TBCHunk(s) in the buffer.
// Remember that this struct must be copyable for CloneReadOnly(). Don't hold
// onto any pointers in here.
// TODO handle destruction of these.
struct SequenceState {
  SequenceState(ProducerID, WriterID, ClientIdentity);
  ~SequenceState();
  SequenceState(const SequenceState&) noexcept;
  SequenceState& operator=(const SequenceState&) noexcept;

  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  ClientIdentity client_identity{};

  // This is semantically a boolean that resets every time BeginRead()
  // increments the generation counter. The semantic is:
  // skip := skip_in_generation == TraceBuffer.read_generation_.
  uint64_t skip_in_generation = 0;

  // When `chunks` becomes empty the SequenceState is eligible to be deleted.
  // Rather than deleting it immediately, we remember its age (derived by
  // incrementing TraceBufferV2.seq_age_) and remove only the oldest entries.
  // See comments in DeleteStaleEmptySequences().
  uint64_t age_for_gc = 0;

  std::optional<ChunkID> last_chunk_id_consumed;

  // This is set whenever a data loss is detected and cleared when reading the
  // next packet for the sequence (which will report previous_packet_dropped).
  bool data_loss = false;

  // An ordered list of chunk offsets, sorted by their ChunkID. Each member
  // corresponsds to the offset within buf_ for the chunk.
  // We store buffer offsets rather than pointers to make buffer cloning easier.
  // This is effectively a deque of TBChunk* (% a call to GetTBChunkAt(off)).
  base::CircularQueue<size_t> chunks;
};

// A packet fragment in the buffer.
// This struct is used in two places:
// 1. When tokenizing the fragments in CopyChunkUntrusted()
// 2. When reading/manipulating the buffer.
struct Frag {
  enum FragType : uint8_t {
    // 1 packet == 1 fragment.
    kFragWholePacket,

    // Fragmentation cases:

    // The last fragment of a chunk, when kLastPacketContinuesOnNextChunk
    kFragBegin,

    // The only fragment of a chunk when both kLastPacketContinuesOnNextChunk &
    // kFirstPacketContinuesFromPrevChunk
    kFragContinue,

    // The first fragment of a chunk when kFirstPacketContinuesFromPrevChunk
    kFragEnd,
  };

  // TODO add ascii diagram.
  // Pointes to the fragment payload, immediately after the header.
  const uint8_t* const begin = nullptr;
  FragType const type = kFragWholePacket;
  uint8_t const hdr_size = 0;
  uint16_t const size = 0;

  uint16_t size_with_header() { return size + hdr_size; }

  Frag(const uint8_t* b, FragType t, uint8_t h, uint16_t s)
      : begin(b), type(t), hdr_size(h), size(s) {}
};

// Iterates over Chunks in a SequenceState
class ChunkSeqIterator {
 public:
  // Rewinds to the first chunk of the sequence.
  explicit ChunkSeqIterator(TraceBufferV2*, SequenceState*);
  ChunkSeqIterator() = default;  // Creates an invalid object, for default init.
  ChunkSeqIterator(const ChunkSeqIterator&) = default;  // Allow copy.
  ChunkSeqIterator& operator=(const ChunkSeqIterator&) = default;

  TBChunk* NextChunkInSequence();
  void EraseCurrentChunk();
  TBChunk* chunk() const { return chunk_; }
  bool sequence_gap_detected() const { return sequence_gap_detected_; }
  bool valid() const { return !!seq_ && !!chunk_; }

 private:
  TraceBufferV2* buf_ = nullptr;
  SequenceState* seq_ = nullptr;
  TBChunk* chunk_ = nullptr;
  bool sequence_gap_detected_ = false;
  size_t list_idx_ = 0;  // Offset of the current chunk in seq_.chunks.
};

// Iterates over fragments of a chunk.
// TODO(minor) add explanation/drawings and mention that is used in two places.
class FragIterator {
 public:
  explicit FragIterator(TBChunk* chunk)
      : chunk_begin_(chunk->fragments_begin()),
        chunk_size_(chunk->payload_size),
        next_frag_off_(chunk->unread_payload_off()),
        chunk_flags_(chunk->flags) {}

  FragIterator(const uint8_t* begin, size_t size, uint8_t flags)
      : chunk_begin_(begin), chunk_size_(size), chunk_flags_(flags) {}

  std::optional<Frag> NextFragmentInChunk();
  size_t next_frag_off() const { return next_frag_off_; }
  bool chunk_corrupted() { return chunk_corrupted_; }
  bool trace_writer_data_drop() { return trace_writer_data_drop_; }

 private:
  const uint8_t* chunk_begin_ = nullptr;
  size_t chunk_size_ = 0;
  size_t next_frag_off_ = 0;
  uint8_t chunk_flags_ = 0;
  bool chunk_corrupted_ = false;
  bool trace_writer_data_drop_ = false;
};

// Identifiers that are constant for a packet sequence.
struct PacketSequenceProperties {
  ProducerID producer_id_trusted;
  ClientIdentity client_identity_trusted;
  WriterID writer_id;

  uid_t producer_uid_trusted() const { return client_identity_trusted.uid(); }
  pid_t producer_pid_trusted() const { return client_identity_trusted.pid(); }
};

// TODO rework comment.
// Takes a chunk in input which is the chunk we want to visit.
// Identifies the first chunk (<= input) in the sequence to start the visit,
// and goes through all fragments in the chunk range.
// It can also go beyond the input (end_) chunk, if the last fragment is a
// fragmented packet that continues beyond.
class ChunkSeqReader {
 public:
  enum Mode { kReadMode, kEraseMode };

  ChunkSeqReader(TraceBufferV2*, TBChunk*, Mode);

  bool ReadNextPacket(TracePacket*);
  TBChunk* end() { return end_; }
  TBChunk* iter() { return iter_; }
  SequenceState* seq() { return seq_; }

 private:
  ChunkSeqReader(const ChunkSeqReader&) = delete;
  ChunkSeqReader& operator=(const ChunkSeqReader&) = delete;
  ChunkSeqReader(ChunkSeqReader&&) = delete;
  ChunkSeqReader& operator=(ChunkSeqReader&&) = delete;

  enum class FragReassemblyResult { kSuccess = 0, kNotEnoughData, kDataLoss };
  FragReassemblyResult ReassembleFragmentedPacket(TracePacket* out_packet,
                                                  Frag* initial_frag);
  void ConsumeFragment(TBChunk*, Frag*);

  TraceBufferV2* const buf_ = nullptr;
  Mode const mode_;

  // This is the chunk passed in the constructor and is our stopping point.
  // It never changes throuhgout the lifetime of ChunkSeqReader.
  // Note that this is NOT the end of the sequence. This is simply where we
  // want to stop iterating, which might be < seq_.end().
  TBChunk* const end_ = nullptr;

  // TODO invalidate this if we delete the seq ?
  SequenceState* const seq_ = nullptr;

  ChunkSeqIterator seq_iter_;

  // This is initially reset to the first chunk of the sequence, and advanced
  // until we hit end_. chunk_ and end_ always belong to the same seq_.
  TBChunk* iter_ = nullptr;

  FragIterator frag_iter_;
};

}  // namespace internal

class TraceBufferV2 : public TraceBuffer {
 public:
  using TBChunk = internal::TBChunk;

  // Import types from the interface to avoid conflicts
  using OverwritePolicy = TraceBuffer::OverwritePolicy;
  using Patch = TraceBuffer::Patch;
  using PacketSequenceProperties = TraceBuffer::PacketSequenceProperties;

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
                          size_t size) override;

  // To read the contents of the buffer the caller needs to:
  //   BeginRead()
  //   while (ReadNextTracePacket(packet_fragments)) { ... }
  // No other calls to any other method should be interleaved between
  // BeginRead() and ReadNextTracePacket().
  // Reads in the TraceBufferV2 are NOT idempotent.
  void BeginRead() override;

  bool ReadNextTracePacket(TracePacket*,
                           PacketSequenceProperties* sequence_properties,
                           bool* previous_packet_on_sequence_dropped) override;

  bool TryPatchChunkContents(ProducerID,
                             WriterID,
                             ChunkID,
                             const Patch* patches,
                             size_t patches_size,
                             bool other_patches_pending) override;

  // Creates a read-only clone of the trace buffer. The read iterators of the
  // new buffer will be reset, as if no Read() had been called. Calls to
  // CopyChunkUntrusted() and TryPatchChunkContents() on the returned cloned
  // TraceBuffer will CHECK().
  std::unique_ptr<TraceBuffer> CloneReadOnly() const override;

  size_t size() const override { return size_; }
  size_t used_size() const override { return used_size_; }
  OverwritePolicy overwrite_policy() const override {
    return overwrite_policy_;
  }
  const TraceStats::BufferStats& stats() const override { return stats_; }
  const WriterStats& writer_stats() const override { return writer_stats_; }
  bool has_data() const override { return used_size_ > 0; }
  void set_read_only() override { read_only_ = true; }

  void DumpForTesting();

 private:
  using Frag = internal::Frag;
  using SequenceState = internal::SequenceState;
  using ChunkSeqReader = internal::ChunkSeqReader;

  friend class TraceBufferV2Test;
  friend class internal::ChunkSeqReader;
  friend class internal::ChunkSeqIterator;

  explicit TraceBufferV2(OverwritePolicy);
  TraceBufferV2(const TraceBufferV2&) = delete;
  TraceBufferV2& operator=(const TraceBufferV2&) = delete;

  // Not using the implicit copy ctor to avoid unintended copies.
  // This tagged ctor should be used only for Clone().
  struct CloneCtor {};
  TraceBufferV2(CloneCtor, const TraceBufferV2&);

  bool Initialize(size_t size);
  TBChunk* CreateTBChunk(size_t off, size_t payload_size);
  void DeleteNextChunksFor(size_t bytes_to_clear);

  void DcheckIsAlignedAndWithinBounds(size_t off) const {
    PERFETTO_DCHECK((off & (alignof(TBChunk) - 1)) == 0);
    PERFETTO_DCHECK(off <= size_ - sizeof(TBChunk));
  }

  // This should only be used when followed by a placement new.
  TBChunk* GetTBChunkAtUnchecked(size_t off) {
    DcheckIsAlignedAndWithinBounds(off);
    return reinterpret_cast<TBChunk*>(begin() + off);
  }

  TBChunk* GetTBChunkAt(size_t off) {
    TBChunk* tbchunk = GetTBChunkAtUnchecked(off);
    PERFETTO_CHECK(tbchunk->outer_size() <= (size_ - off));
    PERFETTO_DCHECK(tbchunk->IsChecksumValid(off));
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

  void DiscardWrite();
  void DeleteStaleEmptySequences();

  uint8_t* begin() const { return reinterpret_cast<uint8_t*>(data_.Get()); }
  uint8_t* end() const { return begin() + size_; }
  size_t size_to_end() const { return size_ - wr_; }

  base::PagedMemory data_;
  size_t size_ = 0;  // Size in bytes of |data_|.

  // High watermark. The number of bytes (<= |size_|) written into the buffer
  // before the first wraparound. This increases as data is written into the
  // buffer and then saturates at |size_|.
  size_t used_size_ = 0;

  size_t wr_ = 0;  // Write cursor (offset since start()).
  size_t rd_ = 0;  // Read cursor. Reset to wr_ on every BeginRead().
  std::optional<ChunkSeqReader> chunk_seq_reader_;

  // Statistics about buffer usage.
  TraceStats::BufferStats stats_;

  // Statistics about TraceWriters.
  WriterStats writer_stats_;

  OverwritePolicy overwrite_policy_ = kOverwrite;

  // Note: we need stable pointers for SequenceState, as they get cached in
  // BufIterator.
  std::unordered_map<ProducerAndWriterID, SequenceState> sequences_;

  // COUNT(sequences_) WHERE sequence.chunks.empty().
  // This is maintained best effort and needs revalidation against sequences_.
  size_t empty_sequences_ = 0;

  // A generation counter incremented every time BeginRead() is called.
  uint64_t read_generation_ = 0;

  // A monotonic counter incremented every time a SequenceState becomes empty.
  // This is used to sort SequenceState by least-recently cleared.
  uint64_t seq_age_ = 0;

  // This buffer is a read-only snapshot obtained via Clone(). If this is true
  // calls to CopyChunkUntrusted() and TryPatchChunkContents() will CHECK().
  bool read_only_ = false;

  // Only used when |overwrite_policy_ == kDiscard|. This is set the first time
  // a write fails because it would overwrite unread chunks.
  bool discard_writes_ = false;

  // When true disable some DCHECKs that have been put in place to detect
  // bugs in the producers. This is for tests that feed malicious inputs and
  // hence mimic a buggy producer.
  bool suppress_client_dchecks_for_testing_ = false;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V2_H_
