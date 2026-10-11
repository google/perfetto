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

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "src/tracing/service/trace_buffer.h"
#include "src/tracing/service/trace_buffer_protovms.h"

namespace perfetto {

class TraceBufferV2;

namespace internal {

class ZstdFrameCodec;

// +---------------------------------------------------------------------------+
// | BundledBuffer                                                             |
// +---------------------------------------------------------------------------+
// A ring buffer of bytes that stores bundles back to back, with no padding
// and no alignment. Bytes are appended at the write cursor |wr_| and consumed
// from the read cursor |rd_|, so the oldest bundle starts at |rd_|. A bundle
// that reaches the end of the memory continues at offset 0, so every read and
// write takes at most two copies.
//
// |wr_| and |rd_| count bytes in the logical stream, with rd_ <= wr_ and
// wr_ - rd_ <= capacity(). The byte at position p is at offset
// p % capacity(). Both cursors reset to 0 when the buffer is drained.
//
//   Positions:
//
//              rd_                                 wr_
//               |                                   |
//               v                                   v
//               [ B3 ][ B4 ][ B5 ][       B6        ]
//
//   The same bytes in memory:
//
//   0                                                            capacity()
//   +----------+----------+------+-----+-----+-------------------+
//   | B6 end   | free     | B3   | B4  | B5  | B6 start          |
//   +----------+----------+------+-----+-----+-------------------+
//              ^ wr_      ^ rd_
//
// TraceBufferV3 appends and consumes whole bundles, so |rd_| always points
// to a bundle header or equals |wr_| when empty.
class BundledBuffer {
 public:
  // Reserves memory for |capacity| stored bytes. Returns false if the
  // allocation fails.
  bool Initialize(size_t capacity);

  // Calls Initialize() with the capacity of |src|, then copies the bytes that
  // |src| stores. Returns false if the allocation fails.
  bool InitializeAsCloneOf(const BundledBuffer& src);

  size_t capacity() const { return mem_.size(); }
  size_t used_size() const { return static_cast<size_t>(wr_ - rd_); }
  size_t free_size() const { return capacity() - used_size(); }
  bool empty() const { return wr_ == rd_; }

  // Writes |size| bytes at |wr_| and advances it. Requires
  // |size| <= free_size().
  void Append(const void* src, size_t size);

  // Copies |size| stored bytes into |dst| and leaves them in the buffer. The
  // first byte copied is the one |offset_from_rd| bytes after |rd_|.
  void CopyOut(size_t offset_from_rd, size_t size, void* dst) const;

  // Returns a pointer to the bytes that CopyOut() would copy, without
  // copying them. Returns nullptr if they wrap around the end of the memory,
  // because then they are not contiguous.
  const uint8_t* PeekContiguous(size_t offset_from_rd, size_t size) const;

  // Removes the |size| oldest bytes by advancing |rd_|. When the buffer
  // becomes empty, resets |wr_| and |rd_| to 0, so that a buffer that is
  // drained often keeps using the pages at its start.
  void Consume(size_t size);

  // If the buffer is empty, advises the OS to release pages past the highest
  // offset written since the last release, with a quarter extra kept for
  // reuse. Does nothing if the buffer holds bundles or if nothing was written
  // since the last release.
  void ReleaseUnusedPages();

 private:
  size_t OffsetOf(uint64_t position) const {
    return static_cast<size_t>(position % mem_.size());
  }

  base::PagedMemory mem_;

  uint64_t wr_ = 0;  // Write cursor. Where the next bundle goes.
  uint64_t rd_ = 0;  // Read cursor. Where the oldest bundle starts.

  // One past the highest byte offset written in |mem_| since
  // ReleaseUnusedPages() last ran on an empty buffer.
  size_t max_written_offset_ = 0;
};

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+
// Memory that bundles are decoded into. ReadNextTracePacket() returns slices
// that point into it.
//
// A read batch starts with BeginRead(), followed by ReadNextTracePacket()
// calls. Allocate() must leave earlier slices valid for the rest of that
// batch. Once the service has finished using them, BeginRead(), EndWriteBatch()
// or MaybeCompact() can call ResetExcept() to reuse the memory.
//
// A read batch can stop in the middle of a bundle. The next batch reads the
// rest of that bundle, so ResetExcept() keeps it decoded.
//
// To avoid a heap allocation per bundle, there are two blocks of |block_size|
// bytes. Allocate() places bundles one after the other in a block, so a block
// can only be freed as a whole. The partly read bundle keeps only its own
// block in use, which leaves the other block free for the next batch.
//
//   Batch 1           block 0 [ A | B ]   Reads A and B, stops inside C.
//                     block 1 [ C ....]
//   ResetExcept(C)    block 0 [ ......]   Block 1 stays because it holds C.
//                     block 1 [ C ....]
//   Batch 2           block 0 [ D | E ]   Reads the rest of C, then D and E.
//                     block 1 [ C ....]
//
// A bundle that fits in neither block gets its own temporary allocation.
// ResetExcept() frees these temporary allocations once their bundles have
// been consumed.
class DecodeArena {
 public:
  explicit DecodeArena(size_t block_size);

  // Returns |size| bytes for one decoded bundle. The bytes stay at the same
  // address until ResetExcept() frees them.
  uint8_t* Allocate(size_t size);

  // Resets the blocks and frees the temporary allocations, except the memory
  // that holds |live_bundle|. Pass the start of the decoded bundle that still
  // has unread packets, or nullptr if there is none. If |live_bundle| is in a
  // block, that whole block stays in use. A block that was reset keeps its
  // memory for the next Allocate().
  void ResetExcept(const uint8_t* live_bundle);

  // Frees the memory of the blocks that hold no allocation.
  void ReleaseEmptyBlocks();

  // An upper bound of the memory in use. It counts both blocks, even before
  // they are allocated, and the temporary allocations that exist now.
  size_t GetMemoryUsageBytes() const;

 private:
  // Heap memory and the number of its bytes that Allocate() handed out.
  // A block has |block_size_| bytes and is allocated on first use.
  // A temporary has exactly |used| bytes.
  struct Allocation {
    std::unique_ptr<uint8_t[]> data;
    size_t used = 0;
  };

  const size_t block_size_;
  std::array<Allocation, 2> blocks_;
  std::vector<Allocation> temporaries_;
};

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+
// A trace buffer that keeps its packets compressed. It reads complete packets
// from a staging TraceBufferV2, compresses them into bundles and stores the
// bundles in a ring buffer of bytes.
//
//   SMB v1 commits, scraping, patches        SMB v2 ring buffer drains
//                 |                                  |
//                 v                                  v
//        +-----------------------------------------------------+
//        | staging buffer: a TraceBufferV2                     |
//        | chunks -> complete packets                          |
//        +-----------------------------------------------------+
//                 |  EndWriteBatch(), when enough chunks arrived
//                 v
//          validate, add loss, compress into bundles
//                 |
//                 v
//        +-----------------------------------------------------+
//        | bundled buffer: a ring buffer of bytes              |
//        | oldest [B1][B2][B3] newest                          |
//        +-----------------------------------------------------+
//                 |  ReadNextTracePacket(): decode the oldest bundle
//                 v
//          complete TracePackets for the service
//
// The staging buffer handles out-of-order chunks, fragments, patches and the
// SMB v2 rewrite. TraceBufferV3 validates each complete packet from staging
// before bundling it. The bundled buffer only stores bytes. See BundleHeader
// below for the format of a bundle.
//
// All calls run on the service thread. A write batch must finish before a
// read starts, and the service must finish using returned slices before the
// next EndWriteBatch(), BeginRead() or MaybeCompact().
//
// Design: RFC 0050, https://github.com/google/perfetto/discussions/7838.
class TraceBufferV3 : public TraceBuffer {
 public:
  // |size_in_bytes| is the capacity of the bundled buffer.
  // Can return nullptr if the memory allocation fails.
  static std::unique_ptr<TraceBufferV3> Create(size_t size_in_bytes,
                                               OverwritePolicy = kOverwrite);

  ~TraceBufferV3() override;

  // The write methods copy into the staging buffer. See TraceBufferV2.
  void CopyChunkUntrusted(ProducerID producer_id_trusted,
                          const ClientIdentity& client_identity_trusted,
                          WriterID writer_id,
                          ChunkID chunk_id,
                          uint16_t num_fragments,
                          uint8_t chunk_flags,
                          bool chunk_complete,
                          const uint8_t* src,
                          size_t size) override;
  bool TryPatchChunkContents(ProducerID,
                             WriterID,
                             ChunkID,
                             const Patch* patches,
                             size_t patches_size,
                             bool other_patches_pending) override;
  bool supports_tracing_v2() const override;
  bool CopyChunkV2Untrusted(const PacketSequenceProperties& sequence,
                            const protozero::ConstBytes* fragments,
                            size_t num_fragments,
                            bool first_continues_from_prev,
                            bool last_continues_on_next) override;
  void RecordChunkV2DataLoss(ProducerID, WriterID) override;
  void RecordAbiViolation() override;

  // Moves the readable packets from staging into bundles, if at least
  // Params::bundle_target_bytes of chunks arrived since the last pass.
  //
  // Bundling cannot happen inside CopyChunkUntrusted(). A scrape copies the
  // chunks of a writer in page order, which can differ from ChunkID order.
  //
  //   CopyChunkUntrusted(W, chunk 7)   A pass here reads chunk 7 after
  //                                    chunk 5 and reports a gap.
  //   CopyChunkUntrusted(W, chunk 6)   Staging then discards chunk 6, which
  //                                    is older than the last chunk read.
  //
  // We count only the bytes that staging accepted, so a duplicate or a
  // rejected chunk does not count. Once a pass finds no more readable
  // packets, it resets the count, even if it read nothing. So a sequence
  // waiting for a patch cannot trigger a pass on every EndWriteBatch().
  void EndWriteBatch() override;

  // Frees the decoded memory of the previous batch, except the bundle that
  // still has unread packets. See DecodeArena::ResetExcept().
  void BeginRead() override;

  // Returns the next packet. The stored bundles come first, then the pending
  // bundle, then the packets that are still in staging.
  //
  // The slices point into DecodeArena and are valid until the next
  // BeginRead(), EndWriteBatch() or MaybeCompact(). A packet larger than
  // kMaxTracePacketSliceSize has more than one slice.
  //
  // If an overwritten or corrupted bundle lost packets of the sequence,
  // |previous_packet_on_sequence_dropped| has those DataLossReason bits,
  // combined with the bits that the packet itself has. The service appends
  // them to the packet and a parser keeps the last value. Otherwise it is 0.
  // Loss that staging detected is already in the packet bytes.
  bool ReadNextTracePacket(
      TracePacket*,
      PacketSequenceProperties* sequence_properties,
      uint32_t* previous_packet_on_sequence_dropped) override;

  // Returns true. We validate each packet before adding it to a bundle, and
  // before appending previous_packet_dropped to it. Otherwise the validator
  // could read the appended field as the missing end of a truncated packet
  // and accept it.
  bool returns_validated_packets() const override;
  uint64_t packets_dropped_by_validation() const override;

  // Releases decode blocks without unread packets, calls MaybeCompact() on
  // staging and releases unused pages of an empty bundled buffer.
  // BeginRead() already frees decoded memory for reuse, so reads do not
  // depend on this call.
  void MaybeCompact() override;

  // The clone owns copies of the stored bundles, the pending bundle, staging,
  // the sequences and the position inside a partly read bundle. Its reads
  // continue where the reads of this buffer stopped.
  //
  // The clone copies only the bytes that the bundled buffer stores and has
  // the capacity of this buffer.
  std::unique_ptr<TraceBuffer> CloneReadOnly() const override;

  TraceBufferProtoVms* protovms() override;

  // Marks staging read-only, so subsequent chunk writes fail a CHECK().
  // Reads can still bundle the packets left in staging.
  void set_read_only() override;

  // The counters of the staging buffer and of the bundled buffer in one
  // BufferStats. See trace_stats.proto for what each one means here.
  // The next stats() call replaces the contents of the returned object.
  const TraceStats::BufferStats& stats() const override;

  // Comes from staging, which sees the chunks of every writer.
  const WriterStats& writer_stats() const override;

  // The capacity of the bundled buffer.
  size_t size() const override;

  // The bytes of the bundles stored in the bundled buffer.
  size_t used_size() const override;

  // The memory of the bundled buffer and of staging, the pending bundle, the
  // ProtoVMs, and an upper bound for the zstd contexts, their scratch and
  // the two decode blocks. The bound includes memory that is allocated on
  // first use. The service sets its watchdog memory limit from this value
  // only at a few events, so it would miss later allocations otherwise.
  size_t GetMemoryUsageBytes() const override;

  // Every chunk goes through staging, which owns the fill policy and tracks
  // whether the buffer has ever received data.
  OverwritePolicy overwrite_policy() const override;
  bool has_data() const override;
  BufType buf_type() const override;

 private:
  friend class TraceBufferV3Test;

  // DefaultParams() derives these sizes from the bundled buffer's capacity.
  // The friend test fixture overrides them to reach the edge cases.
  struct Params {
    // Size of the staging TraceBufferV2. It needs room for the chunks that
    // arrive between two bundling passes. Otherwise staging overwrites or
    // discards chunks before they reach a bundle. TraceBufferV2 rejects a
    // chunk larger than its buffer, so this must be more than the largest SMB
    // chunk.
    size_t staging_bytes = 0;

    // The size that a bundle aims for, before compression. It controls two
    // things.
    // - A bundle is finished when its records reach this size or staging has
    //   no more readable packets. The packet that crosses the target stays
    //   whole.
    // - EndWriteBatch() starts a bundling pass after this many bytes of chunks
    //   arrived in staging, which is about one bundle.
    size_t bundle_target_bytes = 0;

    // Size of each of the two DecodeArena blocks.
    size_t decode_block_bytes = 0;
  };

  // A bundle holds complete packets compressed into one zstd frame.
  //
  //   +--------------+-------------------------------+--------------------+
  //   | BundleHeader | sequence list                 | zstd frame         |
  //   | 12 bytes     | num_sequences entries, each   | the packets,       |
  //   |              | a 4-byte ProducerAndWriterID  | compressed         |
  //   +--------------+-------------------------------+--------------------+
  //   |<-------------------------- bundle_size -------------------------->|
  //
  // The sequence list has one entry for each sequence that has a packet in the
  // bundle.
  //
  // The frame decodes to |uncompressed_size| bytes of records, one per packet.
  //
  //   [sequence_index][packet_size][packet bytes] ...
  //       varint         varint
  //
  // |sequence_index| is the position of the packet's sequence in the sequence
  // list. The packet bytes are a valid TracePacket and already include the loss
  // that staging reported.
  //
  // The header and the sequence list stay outside the frame so that an
  // overwrite can find which sequences lose packets without decoding the
  // bundle.
  //
  // Bundles are stored back to back with no alignment, and a bundle can wrap
  // around the end of the buffer. A BundleHeader in the buffer can therefore
  // start at an odd address or be split in two. So we always memcpy it into a
  // local BundleHeader and never cast a pointer into the buffer.
  struct BundleHeader {
    uint32_t bundle_size = 0;        // Header + sequence list + frame.
    uint32_t uncompressed_size = 0;  // Size of the records in the frame.
    uint32_t num_sequences = 0;      // Entries in the sequence list.
  };
  static_assert(sizeof(BundleHeader) == 12, "Unexpected bundle header layout");
  static_assert(sizeof(ProducerAndWriterID) == 4,
                "Unexpected sequence ID size");

  // Tracks a sequence while at least one bundle holds its packets.
  struct BundledSequence {
    // Copied from the staging buffer when the sequence enters its first bundle.
    // A bundle only stores the ProducerAndWriterID.
    ClientIdentity client_identity;

    // The number of bundles that list this sequence, counting the stored
    // bundles, the pending bundle and the open bundle. The entry is erased
    // when this drops to 0.
    uint32_t bundle_refs = 0;

    // DataLossReason bits for packets that were lost with an overwritten
    // or corrupted bundle. The next packet of the sequence that the consumer
    // reads reports them. We keep these bits here because that packet can
    // already be in a bundle.
    //
    //   Writer W:  [A: overwritten] [B: in a bundle] [C: in staging]
    //
    //   B must report the loss of A. On C it would be too late.
    uint32_t unreported_loss_reasons = 0;

    // The three members below describe this sequence in the open bundle, the
    // one that BuildBundle() is adding packets to. They are kept here so
    // that adding a packet needs no second lookup.
    //
    // They are stale unless |open_bundle_generation| equals the generation of
    // the open bundle, which changes for every bundle. So nothing has to clear
    // them when a bundle is opened. TraceBufferV2 uses the same technique for
    // its read_generation_.
    uint64_t open_bundle_generation = 0;

    // The position of this sequence in the open bundle's sequence list.
    uint32_t index_in_open_bundle = 0;

    // DataLossReason bits that its packets in the open bundle carry.
    uint32_t loss_reasons_in_open_bundle = 0;
  };

  using SequenceMap = std::unordered_map<ProducerAndWriterID, BundledSequence>;

  // The bundle that BuildBundle() is adding packets to. FinishOpenBundle()
  // turns it into a stored bundle or a pending bundle, or drops it.
  struct OpenBundle {
    // Counts the bundles opened so far. See
    // BundledSequence::open_bundle_generation.
    uint64_t generation = 0;

    // The sequences that have a packet in the bundle, in the order in which
    // their first packet arrived.
    std::vector<ProducerAndWriterID> sequence_list;

    // The size of the records added so far.
    uint64_t uncompressed_size = 0;
  };

  // What a read knows about the oldest bundle, the one at the read cursor of
  // the bundled buffer.
  struct OldestBundle {
    // An entry of the oldest bundle's sequence list, resolved once when the
    // bundle is decoded. Returning a packet then needs no map lookup and no
    // unpacking of its ProducerAndWriterID.
    struct ResolvedSequence {
      PacketSequenceProperties properties{};
      BundledSequence* state = nullptr;
    };

    bool is_decoded() const { return records != nullptr; }

    // The offset of the next unread record, starting at 0. It stays valid
    // while the bundle is not decoded, because a clone copies it to skip the
    // packets already read from its source.
    size_t read_offset = 0;

    // The decoded records, in |decode_arena_|. Null until a read needs the
    // bundle. A clone starts with this null too. Removing the bundle clears
    // this pointer, but the bytes stay in |decode_arena_| until the next
    // BeginRead(), EndWriteBatch() or MaybeCompact().
    const uint8_t* records = nullptr;

    // Valid while the bundle is decoded.
    BundleHeader header;

    // The bundle's sequence list. Valid while the bundle is decoded.
    // RemoveOldestBundle() also fills it for a bundle that is not decoded.
    std::vector<ProducerAndWriterID> sequence_list;

    // One entry for each entry of |sequence_list|. Valid while the bundle is
    // decoded.
    std::vector<ResolvedSequence> resolved_sequences;
  };

  // Why RemoveOldestBundle() removes the bundle. It decides what happens to
  // the packets that were not read yet.
  // - kFullyRead: none are left. The consumer read all of them.
  // - kOverwritten: a new bundle needs the space. Their sequences report
  //   DATA_LOSS_OVERWRITE and the ProtoVMs get the packets.
  // - kCorrupted: the bundle cannot be decoded. Every sequence in it reports
  //   DATA_LOSS_CHUNK_CORRUPTED.
  enum class RemoveReason { kFullyRead, kOverwritten, kCorrupted };

  // Why BuildBundle() stopped adding packets.
  enum class BuildResult {
    kTargetReached,   // The records reached Params::bundle_target_bytes.
    kStagingDrained,  // Staging has no more readable packets.
  };

  static Params DefaultParams(size_t size_in_bytes);

  // Creates a buffer with the given sizes. Create() passes DefaultParams().
  // The friend test fixture calls this too, with its own Params.
  static std::unique_ptr<TraceBufferV3> CreateInternal(size_t size_in_bytes,
                                                       OverwritePolicy,
                                                       const Params&);

  explicit TraceBufferV3(const Params&);
  TraceBufferV3(const TraceBufferV3&) = delete;
  TraceBufferV3& operator=(const TraceBufferV3&) = delete;

  // --- Write path: from staging into bundles ---

  // A bundling pass for EndWriteBatch(). Builds bundles until staging has no
  // more readable packets or a bundle becomes pending.
  void BundleAllFromStaging();

  // Updates Counters::bundling_max_us for a pass that began at |start|.
  void RecordBundlingTime(base::TimeNanos start);

  // Opens a bundle, then reads and validates packets from staging and adds
  // them to its zstd frame. Call FinishOpenBundle() after it whatever it
  // returns, because the bundle can have packets even when staging was
  // drained. Once staging is drained, restarts the count of new chunk bytes
  // that EndWriteBatch() checks.
  BuildResult BuildBundle();

  // Returns the entry of |seq_key| in |sequences_|, after adding the sequence
  // to the open bundle if this is its first packet there. The first time a
  // sequence is seen at all, this creates its entry and protects its state
  // from pruning in staging.
  BundledSequence* AddSequenceToOpenBundle(ProducerAndWriterID seq_key,
                                           const ClientIdentity&);

  // Ends the zstd frame of the open bundle. Then stores the bundle in the
  // bundled buffer, keeps it as the pending bundle or drops it. Does nothing
  // if the bundle has no packets.
  void FinishOpenBundle();

  // --- Read path: the oldest bundle ---

  // Builds one bundle for a read. Returns false if staging had no valid
  // packet. Requires an empty bundled buffer and no pending bundle.
  bool BundleOneFromStaging();

  // Copies the header and the sequence list of the oldest bundle out of the
  // bundled buffer. Does not decode or remove anything.
  BundleHeader ReadOldestBundleMetadata(
      std::vector<ProducerAndWriterID>* sequence_list) const;

  // Decompresses the zstd frame of the oldest bundle into |dst|, which has
  // oldest_header.uncompressed_size bytes. Returns false if the frame is not
  // valid or decodes to another size.
  bool DecompressOldestFrame(const BundleHeader& oldest_header, uint8_t* dst);

  // Prepares the oldest bundle for reads and fills |oldest_bundle_|. It reads
  // the header and sequence list, resolves the sequences and decompresses
  // the records. If the bundle is corrupted, removes it and returns false.
  bool LoadOldestBundle();

  // Removes the oldest bundle and drops its references to its sequences. See
  // RemoveReason.
  void RemoveOldestBundle(RemoveReason);

  // Processes the oldest bundle before it is overwritten. Passes its unread
  // packets to the ProtoVMs. If the bundle is partly read and its remaining
  // records are valid, marks loss on exactly the sequences that have unread
  // packets and returns 0. Otherwise returns the DataLossReason bits that the
  // caller must mark on every sequence of the bundle.
  uint32_t ProcessOverwrittenBundle(const BundleHeader& oldest_header);

  // Drops one bundle's reference to a sequence. When no bundle refers to it
  // any more, moves its unreported loss to staging and erases the entry.
  void DropBundleRef(SequenceMap::iterator);

  // The size of |pending_bundle_| in bytes, which is its
  // BundleHeader::bundle_size. Requires a pending bundle.
  size_t PendingBundleSize() const;

  const Params params_;

  std::unique_ptr<TraceBufferV2> staging_buf_;
  internal::BundledBuffer bundled_buf_;

  // Used by both paths. Owns the zstd contexts and the scratch for the
  // compressed frame. It allocates them on first use.
  std::unique_ptr<internal::ZstdFrameCodec> codec_;

  // The sequences with packets in a stored bundle, the pending bundle or
  // the open bundle.
  // We use std::unordered_map so pointers in |oldest_bundle_| stay valid when
  // we add another sequence.
  SequenceMap sequences_;

  // The ProtoVMs get the unread packets of overwritten bundles. They do not
  // see the chunks that staging overwrites. Packets in staging are newer than
  // the ones still in bundles, so a ProtoVM would get them out of order.
  TraceBufferProtoVms protovms_;

  // --- Bundling passes ---

  // Kept across staging reads to reuse the slice vector's allocation.
  TracePacket staged_packet_;

  // stats().bytes_written() of staging when BuildBundle() last found no more
  // readable packets there. EndWriteBatch() counts new chunk bytes from it.
  uint64_t staging_bytes_written_at_last_drain_ = 0;

  // The largest uncompressed_size of the bundles written so far. A decode
  // rejects a header with a larger value before it allocates memory.
  uint32_t max_bundle_uncompressed_size_ = 0;

  OpenBundle open_bundle_;

  // --- Pending bundle, for kDiscard only ---

  // A complete bundle, as BundledBuffer would store it, that did not fit in
  // the free space. Its packets left staging already, so it is read after
  // the stored bundles and before staging.
  std::unique_ptr<uint8_t[]> pending_bundle_;

  // Set when a bundle is kept as pending. EndWriteBatch() then no longer
  // bundles, even after a read makes room.
  bool write_bundling_stopped_ = false;

  // --- Read state ---

  internal::DecodeArena decode_arena_;
  OldestBundle oldest_bundle_;

  // --- Counters ---

  // The counters that this buffer adds to those of staging. See
  // trace_stats.proto for what each one means. CloneReadOnly() copies them
  // all and then resets |bytes_read|, as a clone of TraceBufferV2 resets its
  // read counters.
  struct Counters {
    uint64_t bundles_written = 0;
    uint64_t bundle_bytes_uncompressed = 0;
    uint64_t bundle_bytes_compressed = 0;
    uint64_t bundles_overwritten = 0;
    uint64_t bundles_too_large = 0;
    uint64_t bundles_corrupted = 0;
    uint64_t bundling_max_us = 0;
    uint64_t bytes_read = 0;
    uint64_t packets_dropped_by_validation = 0;
  };
  Counters counters_;

  // The value that stats() returns. Staging owns the chunk counters, so
  // stats() merges them with |counters_| on each call.
  mutable TraceStats::BufferStats stats_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_
