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
class ZstdFrameCodec;

namespace internal {

// +---------------------------------------------------------------------------+
// | BundleBuffer                                                              |
// +---------------------------------------------------------------------------+
// Holds compressed bundles in a ring buffer.
// Push() adds a bundle after the newest one, and Pop() removes the oldest,
// which we call the front bundle.
//
// Bundles are packed one after another with no padding, so a bundle can
// start near the end of the buffer and continue from the start.
//
// Each bundle has four parts.
//
//   +---------------+---------------+-----------------+--------------------+
//   | bundle_header | sequence_keys | sequence_losses | compressed_records |
//   +---------------+---------------+-----------------+--------------------+
//   |<------------------ metadata ------------------->|
//   |<--------------------------- bundle_size ---------------------------->|
//
// - |bundle_header| has four uint32_t fields.
//   - |bundle_size| is the size of the whole bundle, including this header.
//   - |num_sequences| is the number of entries in |sequence_keys|.
//   - |num_losses| is the number of entries in |sequence_losses|.
//   - |records_size| is the size of |compressed_records| once decompressed.
// - |sequence_keys| has the ProducerAndWriterID of each sequence in the bundle.
// - |sequence_losses| has one SequenceLoss for each sequence with nonzero
//   |loss_reasons| on its records.
// - |compressed_records| is one zstd frame that decompresses to
//   |records_size| bytes of records (one per packet).
//
//     [sequence_index][loss_reasons][packet_size][packet_bytes]
//     |<------- record_header (3 varints) ----->|
//
//   - |sequence_index| is the index of the packet's sequence in
//     |sequence_keys|.
//   - |loss_reasons| has the DataLossReason bits for packets we lost on that
//     sequence just before this one.
//     This is kept outside |packet_bytes| because:
//     - Validating before appending the loss field wastes work on packets
//       later overwritten in ring buffer mode.
//     - Appending without validation can make a truncated packet valid.
//       For example, a string missing 3 bytes can consume a 3-byte loss field
//       as payload and pass validation.
//   - |packet_size| is the size of |packet_bytes|.
//
// |sequence_keys| and |sequence_losses| are uncompressed, so an overwrite can
// release the bundle's sequences and report their loss quickly.
//
// BundleBuffer never looks inside |compressed_records|. TraceBufferV3 is
// responsible for writing the records and parsing them back.
class BundleBuffer {
 public:
  // Uncompressed metadata excluding counts and sizes which can be derived.
  struct BundleMetadata {
    // A sequence has at most one entry in |sequence_losses|.
    struct SequenceLoss {
      // A key from |sequence_keys|.
      ProducerAndWriterID sequence_key = 0;

      // The OR of the |loss_reasons| on this sequence's records in the bundle.
      // If we overwrite or drop the bundle unread, the sequence's next packet
      // reports these bits too.
      uint32_t loss_reasons = 0;
    };

    std::vector<ProducerAndWriterID> sequence_keys;
    std::vector<SequenceLoss> sequence_losses;  // Usually empty.
    uint32_t records_size = 0;
  };

  BundleBuffer() = default;
  BundleBuffer(const BundleBuffer&) = delete;
  BundleBuffer& operator=(const BundleBuffer&) = delete;

  // Reserves |capacity| bytes for bundles, or returns false if that fails.
  bool Initialize(size_t capacity);

  // Copies |src|'s bundles into a new allocation of the same capacity, or
  // returns false if allocation fails.
  bool CloneFrom(const BundleBuffer& src);

  // Returns |bundle_size| for a bundle with |num_sequences| sequence keys,
  // |num_losses| loss entries and |compressed_records_size| bytes of
  // compressed records.
  // If any argument exceeds 32 bits, returns UINT64_MAX, which no buffer fits.
  static uint64_t GetBundleSize(size_t num_sequences,
                                size_t num_losses,
                                size_t compressed_records_size);

  // Copies a bundle after the newest one.
  // The bundle must fit in free_size(), so Pop() older bundles first if needed.
  void Push(const BundleMetadata& metadata,
            protozero::ConstBytes compressed_records);

  // Copies the front bundle's metadata into |metadata|, reusing its vectors.
  void PeekFrontMetadata(BundleMetadata* metadata) const;

  // Returns the front bundle's compressed records.
  // The slice can point into the buffer, so use it before Pop().
  Slice PeekFrontCompressedRecords() const;

  void Pop();

  // Releases unused pages at the end of the buffer when it's empty.
  void MaybeCompact();

  size_t capacity() const { return data_.size(); }
  size_t used_size() const { return static_cast<size_t>(end_ - begin_); }
  size_t free_size() const { return capacity() - used_size(); }
  bool empty() const { return begin_ == end_; }

 private:
  struct BundleHeader {
    uint32_t bundle_size = 0;
    uint32_t num_sequences = 0;
    uint32_t num_losses = 0;
    uint32_t records_size = 0;
  };

  // Returns the size of |bundle_header|, |sequence_keys| and |sequence_losses|.
  static uint64_t GetMetadataSize(uint32_t num_sequences, uint32_t num_losses);

  // Appends |size| bytes from |src| to the ring buffer, advancing |end_|.
  // Wraps to the start of |data_| if needed.
  void AppendBytes(const void* src, size_t size);

  BundleHeader PeekFrontHeader() const;

  // Copies |size| bytes to |dst| from |offset_from_front| bytes after |begin_|.
  // Handles wraparound without consuming the bytes or advancing |begin_|.
  void CopyBytes(size_t offset_from_front, size_t size, void* dst) const;

  size_t OffsetOf(uint64_t position) const;

  base::PagedMemory data_;

  // |begin_| and |end_| are positions in the stream of bytes pushed since the
  // buffer was last empty, with begin_ <= end_ <= begin_ + capacity().
  // OffsetOf() maps a position to its offset in |data_|.
  // Both reset to 0 when the buffer empties, so frequent drains reuse the first
  // pages and the next bundle doesn't wrap.
  //
  //   Stream order:  begin_ [ B3 ][ B4 ][ B5 ][       B6        ] end_
  //
  //   Memory order:
  //   0                                                   capacity()
  //   +----------+----------------+------+-----+-----+-------------+
  //   | B6 end   | free           | B3   | B4  | B5  | B6 start    |
  //   +----------+----------------+------+-----+-----+-------------+
  //              ^ OffsetOf(end_) ^ OffsetOf(begin_)
  uint64_t begin_ = 0;  // Start of the front bundle.
  uint64_t end_ = 0;    // End of the newest bundle, where Push() writes next.

  // One past the highest byte written in |data_| since last MaybeCompact().
  size_t max_written_offset_ = 0;

  // Avoids redundant madvise calls on pages already released or never written.
  size_t prev_madvise_begin_ = 0;
};

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+
// Holds the decompressed records of the bundles being read.
// ReadNextTracePacket() returns slices into these records, so they must stay
// alive until the next BeginRead() or MaybeCompact().
//
// Records go into one of two reusable blocks, or into a temporary allocation
// if neither block has room.
//
// A read batch can end partway through a bundle whose compressed copy is
// already gone, so its live records must survive into the next batch.
class DecodeArena {
 public:
  explicit DecodeArena(size_t block_size);
  DecodeArena(const DecodeArena&) = delete;
  DecodeArena& operator=(const DecodeArena&) = delete;

  // Returns |size| bytes, valid until ResetExcept() or CompactExcept() reuses
  // them.
  uint8_t* Allocate(size_t size);

  // Makes the blocks reusable and frees the temporaries, except the one that
  // holds |live_records|, which can be nullptr (indicating no live records).
  // A kept block keeps all its bytes, including records read before
  // |live_records|.
  void ResetExcept(const uint8_t* live_records);

  // Releases unused memory while preserving the allocation with |live_records|.
  // Also frees the block allocations that ResetExcept() keeps for reuse.
  void CompactExcept(const uint8_t* live_records);

  size_t GetMemoryUsageBytes() const;

 private:
  struct Allocation {
    std::unique_ptr<uint8_t[]> data;
    size_t used_size = 0;
  };

  // Capacity of each reusable block.
  const size_t block_size_;

  // Two, so one can keep the live records while the other takes new bundles.
  std::array<Allocation, 2> blocks_;

  // Records that fit in neither block get a temporary of their exact size.
  // TracingServiceImpl::ReadBuffers() doesn't count rejected packets toward its
  // |threshold|, so one call can decode several large bundles.
  // We keep all their temporaries until the next ResetExcept(), because the
  // returned packets point into them.
  std::vector<Allocation> temporaries_;
};

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+
// Compresses trace packets into bundles before storing them.
// A staging TraceBufferV2 stores chunks and reassembles complete packets when
// BuildNextBundle() reads them.
//
//   SMB v1 chunks / SMB v2 fragments
//                   |
//                   v
//   [ staging_buf_: chunks containing packet fragments ]
//                   |
//                   | BuildNextBundle()
//                   |   Read complete packets from staging.
//                   |   Encode records and compress them.
//                   v
//   [ open_bundle_: metadata ] + [ codec_: compressed records ]
//                   |
//                   | CloseOpenBundle()
//                   v
//   [ bundle_buf_: compressed bundles, oldest first ]
//                   |
//                   | DecodeNextBundle()
//                   v
//   [ decode_arena_: decompressed records ]
//   [ decoded_bundle_: sequence state and read cursor ]
//                   |
//                   | ReadNextTracePacket()
//                   v
//   TracePackets with slices into decode_arena_
//
// We bundle packets out of staging in three places.
// - Checkpoint pass: OnWriteCheckpoint(), at a write cycle's end, after at
//   least |target_records_size_| new staging bytes.
// - Space pass: OnStagingNeedsSpace(), when CopyChunkUntrusted() or
//   CopyChunkV2Untrusted() finds no room in staging for the chunk. We bundle
//   the oldest readable packets to make space.
// - Read pass: BundleStagingForRead(), when ReadNextTracePacket() has no
//   decoded, stored or overflow bundle left.
//
// See RFC 0050 at https://github.com/google/perfetto/discussions/7838.
class TraceBufferV3 : public TraceBuffer {
 public:
  // Caps |target_records_size_| at TracingServiceImpl::kWriteIntoFileChunkSize
  // to avoid decoding much more than one read batch when writing to a file.
  // Also sets the minimum |decode_arena_| block size, so multiple small
  // bundles can share a block.
  // A codec sized for this target fits any buffer's bundles, so buffers used on
  // one thread can share one.
  static constexpr size_t kMaxBundleTargetSize = 1024 * 1024;

  // Reserves |size_in_bytes| each for |bundle_buf_| and the staging
  // TraceBufferV2, and compresses bundles with |codec|.
  // Returns nullptr if memory allocation fails.
  static std::unique_ptr<TraceBufferV3> Create(
      size_t size_in_bytes,
      OverwritePolicy,
      std::shared_ptr<ZstdFrameCodec> codec);

  ~TraceBufferV3() override;

  // Uses TraceBufferV2::CopyChunkUntrusted()'s rules for untrusted input,
  // scraped chunks and recommits.
  // If the copy needs space, retries once after OnStagingNeedsSpace().
  // The retry may overwrite old staging chunks under kOverwrite, or reject the
  // chunk and stop accepting writes under kDiscard.
  void CopyChunkUntrusted(ProducerID producer_id_trusted,
                          const ClientIdentity& client_identity_trusted,
                          WriterID writer_id,
                          ChunkID chunk_id,
                          uint16_t num_fragments,
                          uint8_t chunk_flags,
                          bool chunk_complete,
                          const uint8_t* src,
                          size_t size) override;

  // Patches chunks in staging.
  bool TryPatchChunkContents(ProducerID,
                             WriterID,
                             ChunkID,
                             const Patch* patches,
                             size_t patches_size,
                             bool other_patches_pending) override;

  bool supports_tracing_v2() const override;

  // Copies the batch into staging, with the same retry as CopyChunkUntrusted().
  bool CopyChunkV2Untrusted(const PacketSequenceProperties& sequence,
                            const protozero::ConstBytes* fragments,
                            size_t num_fragments,
                            bool first_continues_from_prev,
                            bool last_continues_on_next) override;

  void RecordChunkV2DataLoss(ProducerID, WriterID) override;
  void RecordAbiViolation() override;

  // Bundles readable staging packets when |bytes_until_checkpoint_pass_|
  // reaches zero, after |target_records_size_| new bytes have reached
  // staging. Does nothing after kDiscard stops accepting writes.
  void OnWriteCheckpoint() override;

  // Invalidates earlier slices but keeps the unread part of the decoded bundle.
  void BeginRead() override;

  // Returns the next packet from these sources, in order.
  // 1. Remaining packets in |decoded_bundle_|.
  // 2. Bundles in |bundle_buf_|, oldest first.
  // 3. The retained bundle in |overflow_bundle_|.
  // 4. Packets in |staging_buf_|, bundled and decoded before returning.
  // Returns false if no packet is readable.
  // Returned slices are valid until the next BeginRead() or MaybeCompact().
  //
  // |previous_packet_on_sequence_dropped| includes staging loss and loss from
  // overwritten or dropped bundles.
  bool ReadNextTracePacket(
      TracePacket*,
      PacketSequenceProperties* sequence_properties,
      uint32_t* previous_packet_on_sequence_dropped) override;

  // Frees unused memory in |decode_arena_| and |overwritten_records_|, compacts
  // |staging_buf_| and releases unused pages of |bundle_buf_|.
  void MaybeCompact() override;

  std::unique_ptr<TraceBuffer> CloneReadOnly() const override;

  TraceBufferProtoVms* protovms() override;

  // Later writes, patches and loss reports fail a CHECK, but reads can still
  // bundle the packets left in staging.
  void set_read_only() override;

  const TraceStats::BufferStats& stats() const override;

  const WriterStats& writer_stats() const override;

  // Return |bundle_buf_|'s capacity and used size in bytes, respectively.
  size_t size() const override;
  size_t used_size() const override;

  // Includes staging and decode memory, but not the shared codec.
  size_t GetMemoryUsageBytes() const override;

  OverwritePolicy overwrite_policy() const override;
  bool has_data() const override;
  BufType buf_type() const override;

 private:
  using BundleMetadata = internal::BundleBuffer::BundleMetadata;

  friend class TraceBufferV3Test;

  // Unlike Create(), uses |size_in_bytes| for |bundle_buf_| and takes
  // |target_records_size| from the caller, so tests can control bundle
  // boundaries.
  static std::unique_ptr<TraceBufferV3> CreateInternal(
      size_t size_in_bytes,
      OverwritePolicy,
      size_t target_records_size,
      std::shared_ptr<ZstdFrameCodec> codec);

  TraceBufferV3(size_t target_records_size,
                std::shared_ptr<ZstdFrameCodec> codec);
  TraceBufferV3(const TraceBufferV3&) = delete;
  TraceBufferV3& operator=(const TraceBufferV3&) = delete;

  // Tries to make room for a new chunk by bundling packets from |staging_buf_|.
  // Does not read past SMB v1 gaps that later copies in this cycle may fill.
  // Under kOverwrite, skips scanning |staging_buf_| while
  // |overwrite_bytes_until_space_pass_| is nonzero.
  void OnStagingNeedsSpace();

  // Counts bytes actually written to |staging_buf_| by the copy and any retry.
  // This lets OnWriteCheckpoint() and, under kOverwrite, OnStagingNeedsSpace()
  // wait for enough new bytes without counting rejected copies.
  // If |staging_buf_| stops kDiscard writes, stop bundling from write calls.
  void OnStagingChunkCopyComplete(uint64_t bytes_written_before);

  // Under kDiscard, stops new writes and bundling from write calls.
  // Existing chunks in |staging_buf_| can still be patched and read.
  void StopAcceptingWrites();

  // Bundles readable packets from |staging_buf_| using |read_mode|, then
  // compacts |staging_buf_| once the read finishes.
  void BundleStagingForWrite(internal::ChunkSeqReader::Mode read_mode);

  // Called after |bundle_buf_| and |overflow_bundle_| are drained.
  // Continues the read begun by BeginRead() until |bundle_buf_| has a bundle
  // or no packet is readable, even if kDiscard has stopped writes.
  // Returns whether |bundle_buf_| now has a bundle.
  bool BundleStagingForRead();

  // Records microseconds elapsed since |start| in |bundling_time_us_histogram_|
  // and updates the maximum duration in |v3_stats_|.
  void RecordBundlingTime(base::TimeNanos start);

  // Why BuildNextBundle() stopped reading |staging_buf_|.
  enum class BundleResult {
    // Read enough packets to reach |target_records_size_| record bytes.
    kTargetReached,
    // No more complete packets are readable from |staging_buf_|.
    kStagingReadComplete,
  };

  // Builds one bundle from complete packets in |staging_buf_|.
  // |open_bundle_| holds metadata, while |codec_| receives the record bytes.
  // Calls CloseOpenBundle() once uncompressed record bytes reach
  // |target_records_size_| or no packet is readable.
  BundleResult BuildNextBundle();

  // Finishes the current bundle and clears |open_bundle_|.
  // - With kOverwrite, overwrite the oldest bundles until there is enough room,
  //   then store the new bundle.
  // - With kDiscard, store the bundle if it fits in the free space.
  //   Otherwise, keep it in |overflow_bundle_| and stop accepting writes.
  //   Reads move |overflow_bundle_| into |bundle_buf_| once it empties.
  // - In rare cases, the bundle cannot fit in an empty |bundle_buf_|, or its
  //   uncompressed records exceed UINT32_MAX bytes.
  //   Drop it under either policy and record its loss in |staging_buf_|.
  void CloseOpenBundle();

  // Adds a bundle reference in |bundled_sequences_|.
  // The first reference prevents |staging_buf_| from pruning the sequence.
  void AddSequenceRef(ProducerAndWriterID sequence_key);

  // Releases a bundle reference and carries |loss_reasons| to this sequence's
  // next packet after the released bundle.
  // The last release returns pending loss to |staging_buf_| and allows pruning.
  void ReleaseSequenceRef(ProducerAndWriterID sequence_key,
                          uint32_t loss_reasons);

  // Passes the front bundle to |protovms_|, then removes it from |bundle_buf_|.
  // Decodes it only if a producer in the bundle has a ProtoVM.
  // Its sequences report DATA_LOSS_OVERWRITE and the bundle's recorded loss
  // on their next packet after this bundle.
  void OverwriteFrontBundle();

  // Decompresses the front bundle's records into |dst|.
  void DecompressFrontBundle(uint8_t* dst, size_t dst_size);

  // Decodes the next available bundle and initializes |decoded_bundle_|.
  // Returns false if no packet is readable.
  bool DecodeNextBundle();

  // Target uncompressed record bytes per bundle.
  // OnWriteCheckpoint() waits for this many new bytes in |staging_buf_| before
  // starting a pass, so roughly a bundle's worth of new input has accumulated.
  // BuildNextBundle() closes each bundle at the target, keeping packets whole.
  const size_t target_records_size_;

  // Holds chunks until bundling reads their packets.
  // Reassembles and patches SMB v1 packets, and converts SMB v2 packets to
  // length-delimited protobuf.
  std::unique_ptr<TraceBufferV2> staging_buf_;

  // New bytes needed in |staging_buf_| before OnWriteCheckpoint() can start
  // bundling at the end of a write cycle.
  // Starts at |target_records_size_| and resets to it before each pass and when
  // BundleStagingForRead() finishes reading |staging_buf_|.
  size_t bytes_until_checkpoint_pass_;

  // New |staging_buf_| bytes needed before another OnStagingNeedsSpace() scan.
  // Under kOverwrite, wait for |min_free_bytes| new bytes if the last scan
  // left less free at the write cursor, to avoid rescanning incomplete packets.
  // Writes continue, overwriting old chunks as needed and reducing this count.
  size_t overwrite_bytes_until_space_pass_ = 0;

  // Set once kDiscard stops accepting writes.
  // Reads can still drain accepted packets but cannot enable writes again.
  bool discard_writes_ = false;

  // Lets BundleStagingForRead() skip |staging_buf_| once no packet is readable.
  // BeginRead() clears it so reads can pick up newly available packets.
  bool staging_read_complete_ = false;

  // Reused for |staging_buf_| reads so its slice vector keeps its capacity.
  // Cleared after bundling because compaction can move the referenced chunks.
  TracePacket staged_packet_;

  // Metadata and sequence indexes for the bundle being built.
  // Packet records go directly to |codec_|.
  struct OpenBundle {
    // Sequence keys follow the order of each sequence's first packet.
    // CloseOpenBundle() sets |metadata.records_size| after checking that
    // |records_size| fits in 32 bits.
    BundleMetadata metadata;

    // Maps a sequence key to its index in |metadata.sequence_keys|.
    base::FlatHashMap<ProducerAndWriterID, uint32_t> sequence_indexes;

    // Uncompressed record bytes accumulated in |codec_|.
    // Kept at 64 bits so CloseOpenBundle() can reject a count above UINT32_MAX.
    uint64_t records_size = 0;
  };
  // Empty except while BuildNextBundle() runs.
  OpenBundle open_bundle_;

  // Compresses packet records and decompresses them for reads and |protovms_|.
  // Can be shared with other TraceBufferV3s.
  std::shared_ptr<ZstdFrameCodec> codec_;

  // Stored bundles, oldest first.
  internal::BundleBuffer bundle_buf_;

  // Reuses vector capacity when reading metadata from |bundle_buf_|.
  // Stale once the front bundle changes.
  BundleMetadata front_bundle_metadata_;

  // Keeps a kDiscard bundle that did not fit in |bundle_buf_|'s free space.
  // Its packets already left |staging_buf_|, so keep them until older bundles
  // in |bundle_buf_| have been read.
  struct OverflowBundle {
    BundleMetadata metadata;
    std::vector<uint8_t> compressed_records;
  };
  std::optional<OverflowBundle> overflow_bundle_;

  // Keeps |staging_buf_|'s sequence state alive while bundles reference it.
  // Reads need the ClientIdentity, and |staging_buf_| needs the chunk history
  // to detect gaps and reject duplicate chunks.
  struct BundledSequence {
    // Number of bundles in |bundle_buf_|, |overflow_bundle_| and
    // |decoded_bundle_| that contain this sequence.
    uint32_t bundle_ref_count = 0;

    // DataLossReason bits for this sequence's next decoded bundle.
    // Its packets may already have left |staging_buf_|, so keep the loss here.
    // The last ReleaseSequenceRef() moves these bits back to |staging_buf_|.
    uint32_t unreported_loss_reasons = 0;
  };
  // One entry per sequence referenced by |bundle_buf_|, |overflow_bundle_| or
  // |decoded_bundle_|.
  base::FlatHashMap<ProducerAndWriterID, BundledSequence> bundled_sequences_;

  // Receives packets from overwritten bundles in |bundle_buf_|.
  // Packets lost in |staging_buf_| are newer than retained bundles, so sending
  // them to the VMs here would violate sequence order.
  TraceBufferProtoVms protovms_;

  // Holds records decoded for |protovms_| during bundle overwrites.
  // Grows as needed and is freed by MaybeCompact().
  std::unique_ptr<uint8_t[]> overwritten_records_;
  size_t overwritten_records_capacity_ = 0;

  // Owns the records used by |decoded_bundle_| and returned packet slices.
  // Keeps consumed records alive until BeginRead() or MaybeCompact().
  internal::DecodeArena decode_arena_;

  // Read state for one bundle, set by DecodeNextBundle() and cleared by
  // ReadNextTracePacket() after its last packet.
  struct DecodedBundle {
    struct Sequence {
      PacketSequenceProperties properties{};
      ProducerAndWriterID sequence_key = 0;

      // Loss reported on this sequence's first packet in the bundle.
      // Copied at decode, so overwrites of later bundles don't change it.
      // Cleared by ReadNextTracePacket() after reporting it.
      uint32_t unreported_loss_reasons = 0;
    };

    // Entries follow the order of the bundle's |sequence_keys|.
    std::vector<Sequence> sequences;

    // Points into |decode_arena_|, or is null when no bundle is active.
    const uint8_t* records = nullptr;
    uint32_t records_size = 0;
    // Byte offset of the next record in |records|.
    size_t read_offset = 0;
  };
  DecodedBundle decoded_bundle_;

  // Bundle counters and maximum bundling time.
  // stats() copies these and |bundling_time_us_histogram_| into |stats_|.
  TraceStats::BufferStats::ExperimentalV3Stats v3_stats_;

  // Duration of each bundling pass, in microseconds.
  using BundlingTimeBuckets = Histogram<10, 100, 1000, 10000, 100000>;
  BundlingTimeBuckets bundling_time_us_histogram_;

  // Counts packet bytes and record headers consumed by ReadNextTracePacket().
  // Used for BufferStats.bytes_read.
  uint64_t bytes_read_ = 0;

  // Storage for the reference returned by stats(), rebuilt on every call.
  mutable TraceStats::BufferStats stats_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_
