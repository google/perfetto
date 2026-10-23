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
#include <optional>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/protozero/field.h"
#include "src/tracing/service/histogram.h"
#include "src/tracing/service/trace_buffer.h"
#include "src/tracing/service/trace_buffer_protovms.h"
#include "src/tracing/service/trace_buffer_v2.h"

namespace perfetto {

struct Slice;

namespace internal {

class ZstdFrameCodec;

// +---------------------------------------------------------------------------+
// | BundledBuffer                                                             |
// +---------------------------------------------------------------------------+
// A FIFO of complete bundles stored in a ring buffer.
// The front holds the oldest stored bundle.
// All calls run on the service thread.
// Design: RFC 0050, https://github.com/google/perfetto/discussions/7838.
class BundledBuffer {
 public:
  BundledBuffer() = default;
  BundledBuffer(const BundledBuffer&) = delete;
  BundledBuffer& operator=(const BundledBuffer&) = delete;

  struct BundleMetadata {
    uint32_t uncompressed_size = 0;
    std::vector<ProducerAndWriterID> sequence_list;
  };

  // Reserves memory for |capacity| stored bytes.
  // Returns false if the allocation fails.
  bool Initialize(size_t capacity);

  // Calls Initialize() with the capacity of |src|, then copies its bundles.
  // Returns false if the allocation fails.
  bool CloneFrom(const BundledBuffer& src);

  size_t capacity() const { return data_.size(); }
  size_t used_size() const { return static_cast<size_t>(wr_ - rd_); }
  size_t free_size() const { return capacity() - used_size(); }
  bool empty() const { return wr_ == rd_; }

  // Returns the stored size, including the header, sequence list and frame.
  // Returns UINT64_MAX if either input exceeds 32 bits.
  static uint64_t GetStoredSize(size_t num_sequences, size_t frame_size);

  // Copies a complete bundle into the buffer.
  // Requires GetStoredSize(sequence_list.size(), frame.size) <= free_size().
  void Push(uint32_t uncompressed_size,
            const std::vector<ProducerAndWriterID>& sequence_list,
            protozero::ConstBytes frame);

  // Returns the front bundle's frame.
  // Requires a nonempty buffer and does not remove the bundle.
  // If the frame wraps, the slice owns a contiguous copy, so keep the slice
  // alive while using its bytes.
  // Otherwise the slice borrows the stored bytes until the bundle is removed.
  Slice ReadFrontFrame() const;

  // Reads the front bundle's metadata, reusing the output vector's capacity.
  // Requires a nonempty buffer and does not remove the bundle.
  void ReadFrontMetadata(BundleMetadata* metadata) const;

  // Removes the bundle at the front.
  // Requires a nonempty buffer.
  void Pop();

  // Releases the whole pages after the recently written range, but keeps a
  // spare margin after that range for reuse.
  // Skips pages already released or never written.
  // Does nothing if the buffer holds bundles or no bytes were written since
  // the last call on an empty buffer.
  void MaybeCompact();

 private:
  struct BundleHeader;

  BundleHeader ReadFrontHeader() const;
  void AppendBytes(const void* src, size_t size);
  void CopyBytes(size_t offset_from_tail, size_t size, void* dst) const;

  size_t OffsetOf(uint64_t position) const;

  base::PagedMemory data_;

  // |wr_| and |rd_| count bytes in the logical stream, with rd_ <= wr_ and
  // wr_ - rd_ <= capacity().
  // OffsetOf(p) maps a logical position to p % capacity() in |data_|.
  // Both reset to 0 when empty, so frequent drains reuse the first pages.
  //
  // Logical: rd_ [ B3 ][ B4 ][ B5 ][       B6        ] wr_
  //
  // The same bytes in memory:
  //
  //   0                                                            capacity()
  //   +----------+----------------+------+-----+-----+-------------+
  //   | B6 end   | free           | B3   | B4  | B5  | B6 start    |
  //   +----------+----------------+------+-----+-----+-------------+
  //              ^ OffsetOf(wr_)  ^ OffsetOf(rd_)
  uint64_t wr_ = 0;  // Head, where the next bundle goes.
  uint64_t rd_ = 0;  // Tail, where the front bundle starts.

  // One past the highest byte offset written in |data_| since
  // MaybeCompact() last ran on an empty buffer.
  size_t max_written_offset_ = 0;

  // Whole pages beyond this offset were released or never written at the last
  // compaction.
  // MaybeCompact() raises it to cover subsequent writes before using it.
  size_t prev_madvise_begin_ = 0;
};

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+
// Owns decoded bundle bytes backing the slices from ReadNextTracePacket().
//
// Bundles are decoded in full, but IPC and file reads have a byte threshold.
// A batch can end with packets still unread in the current bundle.
// Those bytes must survive ResetExcept() because the compressed copy is gone.
//
//   After                    block 0     block 1
//   Read A, B, part of C     [ A | B ]   [ C     ]
//   ResetExcept(C)           [ free  ]   [ C     ]
//   Read rest of C, then D   [ D     ]   [ C     ]
//   ResetExcept(nullptr)     [ free  ]   [ free  ]
class DecodeArena {
 public:
  explicit DecodeArena(size_t block_size);
  DecodeArena(const DecodeArena&) = delete;
  DecodeArena& operator=(const DecodeArena&) = delete;

  // Returns |size| writable bytes owned by the arena.
  // Later allocations do not move them.
  // They stay valid until a ResetExcept() or Compact() that does not preserve
  // their allocation.
  uint8_t* Allocate(size_t size);

  // Resets used-byte counts in reusable blocks and frees temporary allocations.
  // Preserves the block or temporary that holds |live_bundle|.
  // Does not clear bytes or release reusable blocks.
  // Pass the unread bundle's Allocate() result, or nullptr if none remains.
  // Call after the previous batch's packet slices are no longer used.
  void ResetExcept(const uint8_t* live_bundle);

  // Calls ResetExcept(), then frees empty blocks.
  void Compact(const uint8_t* live_bundle);

  // Counts both blocks at full capacity, even before allocation, plus
  // the temporary allocations that exist now.
  size_t GetMemoryUsageBytes() const;

 private:
  // A block allocates |block_size_| bytes on first use.
  // A temporary allocates exactly |used_bytes|.
  struct Allocation {
    std::unique_ptr<uint8_t[]> data;

    // Bytes handed out, even for fully read bundles still backing slices.
    size_t used_bytes = 0;
  };

  // Capacity of each reusable block, fixed when the arena is constructed.
  const size_t block_size_;

  // Reused across read batches, holding several bundles each.
  // One can be reset while the other holds an unread bundle.
  std::array<Allocation, 2> blocks_;

  // Bundles that fit in neither reusable block get their own allocation.
  // A bundle can exceed a block, or the current batch can fill both blocks.
  std::vector<Allocation> temporaries_;
};

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+
// A trace buffer that keeps its packets compressed.
// It reads complete packets from a staging TraceBufferV2, compresses them into
// bundles and stores the bundles in a ring buffer of bytes.
//
//   SMB v1 commits, scraping, patches        SMB v2 ring buffer drains
//                 |                                  |
//                 v                                  v
//        +-----------------------------------------------------+
//        | staging buffer: a TraceBufferV2                     |
//        | chunks -> complete packets                          |
//        +-----------------------------------------------------+
//                 |  OnWriteCheckpoint(), after enough chunk bytes
//                 v
//          compress into bundles, loss bits in record headers
//                 |
//                 v
//        +-----------------------------------------------------+
//        | bundled buffer: a ring buffer of bytes              |
//        | oldest [B1][B2][B3] newest                          |
//        +-----------------------------------------------------+
//                 |  ReadNextTracePacket(): decode the front bundle
//                 v
//          complete TracePackets for the service
//
// The staging buffer handles out-of-order chunks, fragments, patches and the
// SMB v2 rewrite.
// A chunk copy that needs staging space also runs a bundling pass, as does a
// read that finds no stored or overflow bundle.
// Loss bits stay in record headers, outside the producer's packet bytes, so
// the service validates exactly what the producer wrote.
// BundledBuffer owns the layout of a stored bundle, and OpenBundle describes
// the records inside its frame.
//
// All calls run on the service thread.
// A write batch must finish before a read starts, and the service must finish
// using returned slices before the next BeginRead() or MaybeCompact().
//
// Design: RFC 0050, https://github.com/google/perfetto/discussions/7838.
class TraceBufferV3 : public TraceBuffer {
 public:
  // |size_in_bytes| sets the capacity of both the bundled and staging buffers.
  // Their storage needs about twice this memory, plus codec and decode storage.
  // Can return nullptr if the memory allocation fails.
  static std::unique_ptr<TraceBufferV3> Create(size_t size_in_bytes,
                                               OverwritePolicy = kOverwrite);

  ~TraceBufferV3() override;

  // The write methods copy into the staging buffer.
  // See TraceBufferV2.
  // Unlike TraceBufferV2, CopyChunkV2Untrusted() accepts producers that have a
  // ProtoVM.
  // Staging has no ProtoVMs, and the ProtoVMs here get only packets from
  // overwritten bundles, which staging already rewrote as length-delimited
  // protobuf.
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

  // Moves readable packets from staging into bundles once staging has stored
  // Params::bundle_target_bytes of new chunks since the last checkpoint pass.
  // |bytes_since_checkpoint_pass_| says what counts.
  // A checkpoint pass resets the count even if it reads nothing, so a sequence
  // waiting for a patch cannot trigger a pass on every OnWriteCheckpoint().
  //
  // Bundling waits for this checkpoint because a scrape copies the chunks of
  // a writer in page order, which can differ from ChunkID order.
  //
  //   CopyChunkUntrusted(W, chunk 7)   A pass here reads chunk 7 after
  //                                    chunk 5 and reports a gap.
  //   CopyChunkUntrusted(W, chunk 6)   Staging then discards chunk 6, which
  //                                    is older than the last chunk read.
  //
  // When a write needs space in staging, a pressure pass tries to bundle
  // readable packets.
  // That pass waits at SMB v1 gaps until the checkpoint.
  void OnWriteCheckpoint() override;

  // Makes the previous batch's decode storage reusable, except the allocation
  // that holds an unread bundle.
  // See DecodeArena::ResetExcept().
  // Starts the staging read for this consumer batch.
  void BeginRead() override;

  // Returns the next packet from the decoded bundle, then stored bundles,
  // the overflow bundle and packets still in staging.
  //
  // The slices point into DecodeArena and are valid until the next
  // BeginRead() or MaybeCompact().
  // A packet larger than kReadSliceSize has more than one slice.
  //
  // If an overwritten or corrupted bundle lost packets of the sequence,
  // |previous_packet_on_sequence_dropped| has those DataLossReason bits,
  // combined with staging loss.
  bool ReadNextTracePacket(
      TracePacket*,
      PacketSequenceProperties* sequence_properties,
      uint32_t* previous_packet_on_sequence_dropped) override;

  // Frees decode blocks and temporaries that hold no unread packets.
  // A freed block allocates its full capacity again on next use.
  // BeginRead() already makes decode storage reusable, so reads do not
  // depend on this call.
  // Compacts staging under either fill policy.
  // Staging holds packets only until they are bundled, so its space is reused
  // under kDiscard too.
  // Then calls MaybeCompact() on the bundled buffer.
  void MaybeCompact() override;

  // The clone owns copies of the stored bundles, the overflow bundle, staging,
  // the sequences and a partly read decoded bundle.
  // Its reads continue where the reads of this buffer stopped.
  //
  // Compressed storage copies only the live bytes and keeps this buffer's
  // capacity.
  // The decoded bundle goes into the clone's own DecodeArena.
  std::unique_ptr<TraceBuffer> CloneReadOnly() const override;

  TraceBufferProtoVms* protovms() override;

  // Marks staging read-only, so later chunk writes and patches fail a CHECK().
  // Reads can still bundle the packets left in staging.
  void set_read_only() override;

  // The counters of the staging buffer and of the bundled buffer in one
  // BufferStats.
  // See trace_stats.proto for what each one means here.
  // The next stats() call replaces the contents of the returned object.
  const TraceStats::BufferStats& stats() const override;

  // Comes from staging, which sees the chunks of every writer.
  const WriterStats& writer_stats() const override;

  // The capacity of the bundled buffer.
  size_t size() const override;

  // The bytes of the bundles stored in the bundled buffer.
  size_t used_size() const override;

  // Includes bundled storage, staging, the overflow bundle, ProtoVMs and
  // temporary decode allocations, plus an upper bound for the zstd contexts,
  // their scratch and the two decode blocks.
  // The bound includes memory that is allocated on first use.
  // The service sets its watchdog memory limit from this value only at a few
  // events, so it would miss later allocations otherwise.
  size_t GetMemoryUsageBytes() const override;

  // Every chunk goes through staging, which owns the fill policy and tracks
  // whether the buffer has ever received data.
  OverwritePolicy overwrite_policy() const override;
  bool has_data() const override;
  BufType buf_type() const override;

 private:
  using BundleMetadata = internal::BundledBuffer::BundleMetadata;

  friend class TraceBufferV3Test;

  // Create() derives these sizes from the bundled buffer's capacity.
  // The friend test fixture overrides them to reach the edge cases.
  struct Params {
    // Size of the staging TraceBufferV2.
    // It must hold the chunks that wait for fragments, patches or missing
    // chunks in the current write batch.
    // It must also fit the largest SMB chunk with its TBChunk header.
    // A smaller staging buffer can need more pressure passes.
    // Under kDiscard, a chunk that still cannot fit after a pass stops intake.
    size_t staging_capacity_bytes = 0;

    // The size that a bundle aims for, before compression.
    // It controls two things.
    // - A bundle is finished when its records reach this size or staging has
    //   no more readable packets.
    //   The packet that crosses the target stays whole.
    // - OnWriteCheckpoint() starts a bundling pass after staging stores this
    //   many chunk bytes, which is about one bundle.
    size_t bundle_target_bytes = 0;
  };

  // Creates a buffer with the given sizes.
  // The friend test fixture calls this too, with its own Params.
  static std::unique_ptr<TraceBufferV3> CreateInternal(size_t size_in_bytes,
                                                       OverwritePolicy,
                                                       const Params&);

  explicit TraceBufferV3(const Params&);
  TraceBufferV3(const TraceBufferV3&) = delete;
  TraceBufferV3& operator=(const TraceBufferV3&) = delete;

  // Copies the chunk into staging without overwriting.
  // If staging needs space, bundles its readable packets and retries once
  // under the fill policy.
  // Under kDiscard, stops intake if the compressed allowance is exhausted or
  // the chunk still cannot fit after the pass.
  // Returns whether staging accepted the chunk.
  bool CopyPreparedChunk(const TraceBufferV2::PreparedChunk&);

  // Stops write-side bundling and makes staging reject subsequent chunks.
  // Reads and patches to retained chunks remain allowed.
  void StopAcceptingWrites();

  // Why a write pass reads staging.
  enum class WriteBundlingReason {
    // A completed write batch can advance past missing SMB v1 chunks.
    kCheckpoint,
    // Makes room for a chunk copy inside a write batch.
    // The pass waits at SMB v1 gaps, because later chunks of the batch can
    // still fill them.
    // A sequence with no history waits for chunk 0.
    // Under kOverwrite, the retry can overwrite chunks that wait at a gap.
    kStagingPressure,
  };

  // Starts a new staging read, in the mode that the reason selects.
  // Bundles readable staging packets until the read completes or kDiscard
  // stops intake.
  // Compacts staging only when the read completes.
  void BundleStagingForWrite(WriteBundlingReason);

  // Produces the next stored bundle, skipping bundles that are too large.
  // Continues the staging read started by BeginRead().
  // Requires an empty bundled buffer and no overflow bundle.
  // Reads accepted staging packets even after kDiscard stops intake.
  // A completed staging read resets |bytes_since_checkpoint_pass_| and
  // compacts staging.
  // Returns whether a stored bundle is ready.
  bool BundleStagingForRead();

  // Records the elapsed time of the bundling pass that started at |start|.
  // Returns the duration in microseconds.
  uint64_t RecordBundlingPass(base::TimeNanos start);

  // Tracks a sequence while at least one bundle holds its packets.
  // While the entry exists, staging does not prune the sequence.
  // This keeps the client identity of bundled packets and the history that
  // staging uses to detect gaps and reject duplicate chunks.
  struct BundledSequence {
    // The number of bundles that list this sequence, counting the stored
    // bundles, the decoded bundle, the overflow bundle and the open bundle.
    // The entry is erased when this drops to 0.
    uint32_t bundle_ref_count = 0;

    // DataLossReason bits from overwritten or corrupted bundles.
    // The next decoded bundle for this sequence takes these bits.
    // If no bundle remains, ReleaseSequenceRef() moves them to staging.
    // Packets in an active decoded bundle keep its earlier loss snapshot.
    // We keep the bits here because newer packets can already be bundled.
    //
    //   Writer W:  [A: overwritten] [B: in a bundle] [C: in staging]
    //
    //   B must report the loss of A.
    //   On C it would be too late.
    uint32_t unreported_loss_reasons = 0;
  };

  // The bundle that BundleNextFromStaging() adds packets to.
  // FinishOpenBundle() stores it, keeps it as overflow, or drops it.
  // Empty between BundleNextFromStaging() calls.
  // The frame decodes to |uncompressed_size| bytes of records, one per packet.
  //
  //   [sequence_index] [loss_reasons] [packet_size] [packet bytes] ...
  //        varint          varint        varint    packet_size bytes
  //
  // |sequence_index| is the position of the packet's sequence in
  // |sequence_list|.
  // |loss_reasons| holds the loss that staging reported for the packet.
  // Packet bytes stay untrusted until the service validates them at readout.
  struct OpenBundle {
    struct Sequence {
      // Position in |sequence_list|.
      uint32_t index = 0;
      // Loss to carry forward if this bundle is dropped.
      uint32_t loss_reasons = 0;
    };

    // The sequences that have a packet in the bundle, in the order in which
    // their first packet arrived.
    std::vector<ProducerAndWriterID> sequence_list;

    // Finds each sequence's index and loss when its packets are interleaved
    // with packets from other sequences.
    base::FlatHashMap<ProducerAndWriterID, Sequence> sequences;

    // The size of the records added so far.
    uint64_t uncompressed_size = 0;
  };

  // Why BundleNextFromStaging() stopped reading, regardless of stored output.
  enum class BundleResult {
    kTargetReached,
    // No packet is readable, even if blocked chunks remain in staging.
    kStagingReadComplete,
  };

  // Builds and finishes one bundle in the current staging read.
  // Both results can produce output or drop a bundle that is too large.
  // kStagingReadComplete also covers an empty staging read.
  BundleResult BundleNextFromStaging();

  // Returns the entry in the open bundle, adding it on first use.
  // The first use acquires one reference in |bundled_sequences_|.
  // Creating the |bundled_sequences_| entry protects the sequence from
  // pruning in staging.
  // The pointer is valid until another sequence is added or the bundle
  // finishes.
  OpenBundle::Sequence* GetOrAddOpenBundleSequence(ProducerAndWriterID seq_key);

  // Adds |loss_reasons| and drops one bundle's reference to the sequence.
  // When no bundle refers to it, moves its unreported loss to staging, lets
  // staging prune the sequence again and erases the entry.
  // Skips missing entries in corrupted bundles.
  void ReleaseSequenceRef(ProducerAndWriterID seq_key, uint32_t loss_reasons);

  struct OverflowBundle {
    BundleMetadata metadata;
    std::vector<uint8_t> frame;
  };

  // Stores the open bundle, evicting the oldest bundles under kOverwrite.
  // Drops a bundle that is too large for the buffer.
  // Under kDiscard, each stored or overflow bundle counts once toward the
  // compressed allowance.
  // Once the allowance is exhausted, intake stops, but accepted packets stay
  // readable.
  // A kDiscard bundle that does not fit in the free space becomes the overflow
  // bundle.
  // Clears construction state before returning and keeps allocated capacity.
  // An empty bundle produces no output.
  void FinishOpenBundle();

  // Decompresses the open bundle and adds each packet's
  // previous_packet_dropped bits to its sequence's |loss_reasons|.
  // This preserves producer loss when a bundle is too large to store.
  // If decompression fails, no producer loss is recovered.
  void RecoverOpenBundleProducerLoss(protozero::ConstBytes frame);

  // Why RemoveFrontBundle() removes the bundle.
  // It decides what happens to the packets that were not read yet.
  // - kOverwritten: a new bundle needs the space.
  //   Their sequences report DATA_LOSS_OVERWRITE and the ProtoVMs get the
  //   packets.
  // - kCorrupted: the bundle cannot be decoded.
  //   Every sequence in it reports DATA_LOSS_CHUNK_CORRUPTED.
  enum class RemoveReason { kOverwritten, kCorrupted };

  // Removes the front bundle and drops its references to its sequences.
  // See RemoveReason.
  void RemoveFrontBundle(RemoveReason);

  // Passes the packets of the front bundle to the ProtoVMs of their
  // producers before an overwrite removes it.
  // A decoded bundle has already left storage, so this path never sends
  // its unread packets to the ProtoVMs.
  void ProcessOverwrittenBundle(const BundleMetadata& metadata);

  // The largest slice that ReadNextTracePacket() returns.
  // Tests check that it fits the service's consumer slice limit.
  static constexpr size_t kReadSliceSize = 128 * 1024 - 512;

  // The consumer's bundle, removed from compressed storage after decode.
  struct DecodedBundle {
    // An entry of the decoded bundle's sequence list, looked up once when the
    // bundle is decoded.
    // Returning a packet then needs no map lookup and no unpacking of its
    // ProducerAndWriterID.
    struct Sequence {
      PacketSequenceProperties properties{};
      ProducerAndWriterID seq_key = 0;
      // Loss that preceded this bundle, cleared on this sequence's first read.
      // Record loss stays separate until ReadNextTracePacket() returns it.
      // Overwrites of newer stored bundles must not change this snapshot.
      uint32_t loss_reasons = 0;
    };

    // The offset of the next unread record, starting at 0.
    size_t read_offset = 0;

    // The decoded records, in |decode_arena_|.
    // Null when no bundle is active.
    // A clone copies the decoded bytes.
    // Finishing the bundle clears this pointer, but the bytes stay in
    // |decode_arena_| until the next BeginRead() or MaybeCompact().
    const uint8_t* records = nullptr;

    // Decoded size, or 0 when no bundle is active.
    uint32_t uncompressed_size = 0;

    // One entry for each entry of the bundle's sequence list.
    // Empty when no bundle is active.
    std::vector<Sequence> sequences;
  };

  // Decodes the next bundle from the stored bundles, then the overflow
  // bundle, then staging.
  // Skips corrupted bundles and returns whether a decoded bundle is ready.
  // Requires no active decoded bundle.
  bool ReadNextBundle();

  // Checks metadata and decodes the front bundle into |decoded_bundle_|.
  // ReadNextTracePacket() checks each record before returning its packet.
  // Pops the compressed copy after decoding.
  // On corruption, removes the bundle, records DATA_LOSS_CHUNK_CORRUPTED for
  // its sequences and returns false without changing |decoded_bundle_|.
  bool DecodeFrontBundle();

  // Drops the decoded bundle's references to its sequences.
  // Each sequence gets back the snapshot loss that none of its packets
  // reported, plus |loss_reasons|.
  // See ReleaseSequenceRef().
  // Runs after the last record, or when a record is malformed.
  // Clears the decoded bundle's state.
  // The arena keeps returned slices alive until the next BeginRead() or
  // MaybeCompact().
  void ReleaseDecodedBundle(uint32_t loss_reasons);

  // Counts bundle storage, bundling time and bytes returned to the consumer.
  // See trace_stats.proto for the published counters.
  // CloneReadOnly() copies them all and then resets |bytes_read|, as a clone
  // of TraceBufferV2 resets its read counters.
  struct BundleCounters {
    uint64_t bundles_written = 0;
    uint64_t bundle_bytes_uncompressed = 0;
    // Counts each stored or overflow bundle's header, sequence list and frame.
    // Reads never reduce this total.
    // Under kDiscard, the total counts against the compressed allowance,
    // which equals the buffer capacity.
    // Intake stops when the total reaches it.
    uint64_t bundle_bytes_stored = 0;
    // Largest uncompressed size of a stored or overflow bundle.
    // Bounds allocations when decoding stored headers.
    uint32_t max_bundle_uncompressed_size = 0;

    uint64_t bundles_overwritten = 0;
    uint64_t bundles_too_large = 0;
    uint64_t bundles_corrupted = 0;

    uint64_t bundling_max_us = 0;
    Histogram<10, 100, 1000, 10000, 100000> bundling_time_us;

    uint64_t staging_pressure_passes = 0;
    uint64_t staging_pressure_time_us = 0;
    uint64_t staging_pressure_compactions_skipped = 0;

    uint64_t bytes_read = 0;
  };

  const Params params_;

  // Storage for producer chunks and compressed bundles.
  std::unique_ptr<TraceBufferV2> staging_buf_;
  internal::BundledBuffer bundled_buf_;
  // Reusable scratch populated by each ReadFrontMetadata() call.
  // Its contents can be stale after the front bundle changes.
  BundleMetadata front_bundle_metadata_;
  // kDiscard only.
  // A complete bundle that did not fit in the free space.
  // Its packets left staging already, so it is read after the stored bundles
  // and before staging.
  std::optional<OverflowBundle> overflow_bundle_;

  // Shared state for bundle creation, removal and readout.
  // The codec owns the zstd contexts and compressed-frame scratch.
  // It allocates them on first use.
  std::unique_ptr<internal::ZstdFrameCodec> codec_;
  // The sequences with packets in stored, decoded, overflow or open bundles.
  // TODO(sashwinbalaji): Measure CPU time and retained memory for this map and
  // |open_bundle_.sequences|, including bundle resets, writer churn and
  // cloning.
  base::FlatHashMap<ProducerAndWriterID, BundledSequence> bundled_sequences_;
  // The ProtoVMs get the unread packets of overwritten bundles.
  // They do not see the chunks that staging overwrites.
  // Packets in staging are newer than the ones still in bundles, so a ProtoVM
  // would get them out of order.
  TraceBufferProtoVms protovms_;

  // The current bundle build and write-side scheduling.
  OpenBundle open_bundle_;
  // Kept across staging reads to reuse the slice vector's allocation.
  // Holds no slices between bundle builds.
  TracePacket staged_packet_;
  // Bytes of the new TBChunks that staging stored for any producer since the
  // last checkpoint pass, taken from staging's bytes_written.
  // In-place recommits and rejected chunks add none.
  // A relocated recommit counts, because staging writes a new TBChunk.
  // Stops counting once the bundle target is reached.
  // Consumer readout also resets this when staging has no readable packets.
  // Pressure passes preserve it so the completed batch's checkpoint can
  // handle gaps that the pressure pass left behind.
  size_t bytes_since_checkpoint_pass_ = 0;
  // kDiscard stops intake permanently when the compressed allowance is
  // exhausted, or a chunk still cannot fit after a pressure pass.
  // This flag stops write-side bundling, while staging's flag rejects chunks.
  // CopyPreparedChunk() and StopAcceptingWrites() keep the flags in sync.
  // Reads never reopen intake.
  bool discard_writes_ = false;

  // Bundle decoding and consumer readout.
  internal::DecodeArena decode_arena_;
  DecodedBundle decoded_bundle_;
  // No more staging packets are readable in this consumer batch.
  // BundleStagingForRead() compacts staging when it sets this, which
  // invalidates the staging cursor.
  // Only BeginRead() starts a new staging read and clears this.
  bool staging_read_complete_ = false;

  // Statistics for bundles and consumer readout.
  BundleCounters bundle_counters_;
  // The value that stats() returns.
  // Staging owns the chunk counters, so stats() merges them with
  // |bundle_counters_| on each call.
  mutable TraceStats::BufferStats stats_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_
