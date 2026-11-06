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

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_stats.h"
#include "perfetto/protozero/field.h"
#include "src/tracing/service/block_allocator.h"
#include "src/tracing/service/histogram.h"
#include "src/tracing/service/sequence_state_map.h"
#include "src/tracing/service/trace_buffer.h"
#include "src/tracing/service/trace_buffer_protovms.h"
#include "src/tracing/service/trace_buffer_v2.h"

namespace perfetto {

class ZstdFrameCodec;

namespace internal {

// +---------------------------------------------------------------------------+
// | BundleBuffer                                                              |
// +---------------------------------------------------------------------------+
// Holds compressed bundles in a ring buffer, with the oldest at the front.
//
// Bundles are packed one after another with no padding, so a bundle can
// start near the end of the buffer and continue from the start.
//
// Each bundle has two parts.
//
//   +---------------+------------------------+
//   | bundle_header | compressed_packet_data |
//   +---------------+------------------------+
//   |<-------------- bundle_size ----------->|
//
// - |bundle_header| has four uint32_t fields.
//   - |bundle_size| is the size of the whole bundle, including this header.
//   - |bundle_id| uniquely identifies the bundle among stored bundles.
//   - |num_sequences| is the number of distinct sequences in the bundle.
//     Stored in the header so an overwrite doesn't have to decompress the
//     bundle.
//   - |packet_data_size| is the uncompressed size of |packet_data|.
// - |compressed_packet_data| is one zstd frame.
//   It decompresses to |packet_data|, a sequence of packets with their headers.
//
//     [sequence_index][packet_size][packet_bytes]
//     |<--bundle_packet_header--->|
//
//   - |bundle_packet_header| contains two varints.
//     - |sequence_index| identifies the packet's sequence within the bundle,
//       from 0 to |num_sequences| - 1.
//     - |packet_size| is the size of |packet_bytes|.
//   - |packet_bytes| contains the protobuf packet, including any appended loss
//     field.

// The header stored before each bundle's compressed packet data.
struct BundleHeader {
  uint32_t bundle_size = 0;
  uint32_t bundle_id = 0;
  uint32_t num_sequences = 0;
  uint32_t packet_data_size = 0;
};

class BundleBuffer {
 public:
  BundleBuffer() = default;
  BundleBuffer(const BundleBuffer&) = delete;
  BundleBuffer& operator=(const BundleBuffer&) = delete;

  // Reserves |capacity| bytes for bundles, or returns false if that fails.
  bool Initialize(size_t capacity);

  // Copies |src|'s bundles into a new allocation of the same capacity, or
  // returns false if allocation fails. This buffer must be empty.
  bool CloneFrom(const BundleBuffer& src);

  // Copies a bundle after the newest one.
  // The bundle must fit in free_size().
  void PushBack(uint32_t num_sequences,
                uint32_t packet_data_size,
                protozero::ConstBytes compressed_packet_data);

  BundleHeader PeekFrontHeader() const;

  // Returns the front bundle's compressed packet data.
  // The returned slice is valid until the next PopFront().
  Slice PeekFrontCompressedPacketData() const;

  void PopFront();

  // Releases unused pages at the end of the buffer when it's empty.
  void MaybeCompact();

  size_t capacity() const { return data_.size(); }
  size_t used_size() const { return static_cast<size_t>(end_ - begin_); }
  size_t free_size() const { return capacity() - used_size(); }
  bool empty() const { return begin_ == end_; }

  // The id PushBack() stores in the next bundle's header.
  uint32_t next_bundle_id() const { return next_bundle_id_; }

 private:
  // Appends |size| bytes from |src| to the ring buffer, advancing |end_|.
  // Wraps to the start of |data_| if needed.
  void AppendBytes(const void* src, size_t size);

  // Copies |size| bytes to |dst| from |offset_from_front| bytes after |begin_|.
  // Handles wraparound and doesn't consume the bytes.
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
  uint64_t end_ = 0;    // End of the newest bundle, where PushBack() writes.

  // Monotonic counter that gives the next pushed bundle its id.
  uint32_t next_bundle_id_ = 0;

  // One past the highest byte written in |data_| since last MaybeCompact().
  size_t max_written_offset_ = 0;

  // Avoids redundant madvise calls on pages already released or never written.
  size_t prev_madvise_begin_ = 0;
};

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+
// Compresses trace packets into bundles before storing them.
// A staging TraceBufferV2 stores chunks and reassembles complete packets when
// FillNextBundle() reads them.
//
//   SMB v1 chunks / SMB v2 fragments
//                   |
//                   v
//   [ staging_buf_: chunks containing packet fragments ]
//                   |
//                   | FillNextBundle()
//                   |   Read complete packets from staging.
//                   |   Encode packets with headers and compress them.
//                   v
//   [ open_bundle_: metadata ] + [ codec_: compressed packet data ]
//                   |
//                   | CloseOpenBundle()
//                   v
//   [ bundle_buf_: compressed bundles, oldest first ]
//                   |
//                   | DecodeNextBundle()
//                   v
//   [ packet_data_allocator_: decompressed packet data ]
//   [ decoded_bundle_: sequence state and read cursor ]
//                   |
//                   | ReadNextTracePacket()
//                   v
//   TracePackets with slices into packet_data_allocator_
//
// We bundle packets out of staging in three places.
// - Checkpoint pass: OnWriteCheckpoint(), at a write cycle's end, once staging
//   holds at least |target_packet_data_size_| bytes.
// - Space pass: MaybeFreeStagingSpace(), before a copy that might wrap or
//   overwrite staging. We bundle all readable packets to make space.
// - Read pass: BundleStagingForRead(), when ReadNextTracePacket() has no
//   decoded or stored bundle left.
//
// See RFC 0050 at https://github.com/google/perfetto/discussions/7838.
class TraceBufferV3 : public TraceBuffer {
 public:
  // Caps |target_packet_data_size_|, so a read decodes about one
  // TracingServiceImpl::kWriteIntoFileChunkSize batch at a time.
  // Also sizes shared codecs and the smallest |packet_data_allocator_| block.
  static constexpr size_t kMaxBundleTargetSize = 1024 * 1024;

  // Reserves |size_in_bytes| each for |bundle_buf_| and the staging
  // TraceBufferV2, and compresses bundles with |codec|.
  // Returns nullptr if memory allocation fails.
  static std::unique_ptr<TraceBufferV3> Create(
      size_t size_in_bytes,
      OverwritePolicy,
      std::shared_ptr<ZstdFrameCodec> codec);

  ~TraceBufferV3() override;

  // Calls TraceBufferV2::CopyChunkUntrusted() on |staging_buf_|, after bundling
  // readable packets if the copy might wrap or overwrite staging chunks.
  // If it still doesn't fit, kOverwrite overwrites old staging chunks and
  // kDiscard rejects the chunk and stops accepting writes.
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

  // Same as CopyChunkUntrusted(), using TraceBufferV2::CopyChunkV2Untrusted().
  bool CopyChunkV2Untrusted(const PacketSequenceProperties& sequence,
                            const protozero::ConstBytes* fragments,
                            size_t num_fragments,
                            bool first_continues_from_prev,
                            bool last_continues_on_next) override;

  void RecordChunkV2DataLoss(ProducerID, WriterID) override;
  void RecordAbiViolation() override;

  // Bundles readable staging packets once staging holds
  // |target_packet_data_size_| bytes.
  // Does nothing after kDiscard stops accepting writes.
  void OnWriteCheckpoint() override;

  // Invalidates earlier slices but keeps the unread part of the decoded bundle.
  void BeginRead() override;

  // Returns the next packet from these sources, in order.
  // 1. Remaining packets in |decoded_bundle_|.
  // 2. Bundles in |bundle_buf_|, oldest first.
  // 3. Remaining packets in |staging_buf_|.
  // Returns false if no packet is readable.
  // Returned slices are valid until the next BeginRead() or MaybeCompact().
  bool ReadNextTracePacket(
      TracePacket*,
      PacketSequenceProperties* sequence_properties,
      uint32_t* previous_packet_on_sequence_dropped) override;

  // Reclaims memory left over from earlier reads and write bursts.
  // Invalidates earlier slices but keeps the unread part of the decoded bundle.
  // - |staging_buf_|: moves its unread chunks to the front and releases unused
  //   pages.
  // - |bundle_buf_|: releases unused pages, if it holds no bundles.
  // - |returned_packet_data_|: frees it, since it is no longer needed.
  // - |open_bundle_| and |bundle_sequences_|: frees the capacity their maps
  //   kept after a burst, if they hold no entries.
  void MaybeCompact() override;

  std::unique_ptr<TraceBuffer> CloneReadOnly() const override;

  TraceBufferProtoVms* protovms() override;

  // Later writes, patches and loss reports fail a CHECK, but reads can still
  // bundle the packets left in staging.
  void set_read_only() override;

  const TraceStats::BufferStats& stats() const override;

  const WriterStats& writer_stats() const override;

  // Returns |bundle_buf_|'s capacity and used size in bytes, respectively.
  size_t size() const override;
  size_t used_size() const override;

  // Counts the bundle and staging buffers, ProtoVM memory and the blocks that
  // hold decompressed packet data.
  size_t GetMemoryUsageBytes() const override;

  OverwritePolicy overwrite_policy() const override;
  bool has_data() const override;
  BufType buf_type() const override;

 private:
  friend class TraceBufferV3Test;

  // Takes the exact |bundle_buf_| size and bundle target, so tests can control
  // bundle boundaries.
  static std::unique_ptr<TraceBufferV3> CreateInternal(
      size_t size_in_bytes,
      OverwritePolicy,
      size_t target_packet_data_size,
      std::shared_ptr<ZstdFrameCodec> codec);

  TraceBufferV3(size_t target_packet_data_size,
                std::shared_ptr<ZstdFrameCodec> codec);
  TraceBufferV3(const TraceBufferV3&) = delete;
  TraceBufferV3& operator=(const TraceBufferV3&) = delete;

  // Called before each copy into |staging_buf_|, with the most payload bytes
  // that copy can store.
  // If the copy might wrap or overwrite unread chunks, bundles all readable
  // packets first to free their space.
  void MaybeFreeStagingSpace(size_t max_payload_size);

  // True once kDiscard has stopped accepting writes.
  bool discard_writes() const { return staging_buf_->discard_writes_; }

  // Under kDiscard, stops new writes and bundling from write calls.
  // Existing chunks in |staging_buf_| can still be patched and read.
  void StopAcceptingWrites() { staging_buf_->StopAcceptingWrites(); }

  // Called by OnWriteCheckpoint() and MaybeFreeStagingSpace().
  // Bundles readable packets from |staging_buf_|, then compacts |staging_buf_|
  // once the read finishes.
  void BundleStagingForWrite();

  // Called after |bundle_buf_| is drained.
  // Continues the read begun by BeginRead() until |bundle_buf_| has a bundle
  // or no packet is readable, even if kDiscard has stopped writes.
  // Returns whether |bundle_buf_| now has a bundle.
  bool BundleStagingForRead();

  void RecordBundlingTime(base::TimeNanos duration);

  internal::SequenceStateMap& sequence_states() {
    return staging_buf_->sequence_states_;
  }

  // One sequence of one bundle.
  struct BundleSequence {
    // Keeps the sequence's state until the bundle leaves |bundle_buf_|.
    // Bundles don't store their writers, so reads and overwrites get the
    // sequence's identity from here.
    internal::SequenceState::Lease lease;

    // The OR of the loss staging reported on this sequence's packets.
    // If we overwrite or drop the bundle unread, the sequence's next packet
    // reports these bits too.
    uint32_t loss_reasons = 0;
  };

  // Returns the entry for |sequence_index| in bundle |bundle_id|.
  // A bundle keeps its entries until it leaves |bundle_buf_|, so a miss means a
  // bug.
  BundleSequence& GetBundleSequence(uint32_t bundle_id,
                                    uint32_t sequence_index);

  // Builds one bundle from complete packets in |staging_buf_|.
  // |open_bundle_| holds metadata, while |codec_| receives the packet data.
  // Calls CloseOpenBundle() once the packet data reaches
  // |target_packet_data_size_| or no packet is readable.
  // Returns false if staging ran out of packets before the bundle filled.
  bool FillNextBundle();

  // Called by FillNextBundle() to store the finished bundle and clear
  // |open_bundle_|.
  // - kOverwrite overwrites the oldest bundles until the new one fits.
  // - kDiscard drops the bundle and stops accepting writes if it doesn't fit in
  //   the free space.
  // - Either policy drops a bundle that can't fit in an empty |bundle_buf_|, or
  //   whose uncompressed packet data exceeds UINT32_MAX bytes.
  // A dropped bundle's loss goes on the next packet from |staging_buf_|.
  void CloseOpenBundle();

  // Passes the front bundle to |protovms_|, then removes it from |bundle_buf_|.
  // Decodes it only if a producer in the bundle has a ProtoVM.
  // Its sequences report DATA_LOSS_OVERWRITE and the bundle's recorded loss
  // on their next packet after this bundle.
  void OverwriteFrontBundle();

  // Decompresses the front bundle's packet data into |dst|.
  void DecompressFrontBundle(uint8_t* dst, size_t dst_size);

  // Decodes the next available bundle and initializes |decoded_bundle_|.
  // Returns false if no packet is readable.
  bool DecodeNextBundle();

  // Target uncompressed packet data bytes per bundle.
  // FillNextBundle() closes each bundle at the target, keeping packets whole.
  const size_t target_packet_data_size_;

  // Holds chunks until bundling reads their packets.
  // Reassembles and patches SMB v1 packets, and converts SMB v2 packets to
  // length-delimited protobuf.
  // It also owns the sequence state map.
  // Declared before |bundle_sequences_|, because a lease calls into its
  // SequenceState when it ends.
  std::unique_ptr<TraceBufferV2> staging_buf_;

  // Lets BundleStagingForRead() skip |staging_buf_| once no packet is readable.
  // BeginRead() clears it so reads can pick up newly available packets.
  bool staging_read_complete_ = false;

  // Reused for |staging_buf_| reads so its slice vector keeps its capacity.
  // Cleared after bundling because compaction can move the referenced chunks.
  TracePacket staged_packet_;

  // State for the bundle being built.
  // Packet data goes directly to |codec_|.
  struct OpenBundle {
    // Maps each sequence in this bundle to its |sequence_index|, so a writer
    // that comes back later in the bundle reuses its index.
    // Indexes follow the order of each sequence's first packet.
    base::FlatHashMap<ProducerAndWriterID, uint32_t> sequence_indexes;

    // Uncompressed packet data bytes accumulated in |codec_|.
    // Kept at 64 bits so CloseOpenBundle() can reject a count above UINT32_MAX.
    uint64_t packet_data_size = 0;
  };
  // Empty except while FillNextBundle() runs.
  OpenBundle open_bundle_;

  // Compresses packet data and decompresses it for reads and |protovms_|.
  // Can be shared with other TraceBufferV3s.
  std::shared_ptr<ZstdFrameCodec> codec_;

  // Stored bundles, oldest first.
  internal::BundleBuffer bundle_buf_;

  // Bundle id and the sequence's index in that bundle.
  using BundleSequenceKey = std::pair<uint32_t, uint32_t>;

  // Holds one entry for each sequence of the open bundle and of each bundle in
  // |bundle_buf_|.
  // An entry and its lease stay here until the bundle leaves |bundle_buf_|.
  base::FlatHashMap<BundleSequenceKey, BundleSequence> bundle_sequences_;

  // Receives packets from overwritten bundles in |bundle_buf_|.
  // Packets lost in |staging_buf_| are newer than retained bundles, so sending
  // them to the VMs here would violate sequence order.
  TraceBufferProtoVms protovms_;

  // Owns decompressed packet data, for reads and for overwritten bundles
  // passed to |protovms_|.
  // Declared before |returned_packet_data_| and |decoded_bundle_|, because
  // their Allocations must be destroyed first.
  internal::BlockAllocator packet_data_allocator_;

  // Packet data of fully read bundles, which returned slices still point into.
  // Kept until BeginRead() or MaybeCompact().
  std::vector<internal::BlockAllocator::Allocation> returned_packet_data_;

  // The bundle being read, which has already left |bundle_buf_|.
  // Set by DecodeNextBundle() and cleared by ReadNextTracePacket() after all
  // its packets are read.
  struct DecodedBundle {
    struct Sequence {
      PacketSequenceProperties properties{};

      // Loss reported on this sequence's first packet in the bundle.
      // Copied at decode, so overwrites of later bundles don't change it.
      // Cleared by ReadNextTracePacket() after reporting it.
      uint32_t unreported_loss_reasons = 0;
    };

    // Indexed by |sequence_index|.
    std::vector<Sequence> sequences;

    // Keeps the decoded bytes alive until we finish this bundle.
    // Empty when no bundle is being read.
    // A read batch can end partway through a bundle whose compressed copy is
    // already gone, so these bytes must survive into the next batch.
    internal::BlockAllocator::Allocation packet_data;
    // Byte offset of the next packet in |packet_data|.
    size_t read_offset = 0;
  };
  DecodedBundle decoded_bundle_;

  // Bundle counters and maximum bundling time.
  TraceStats::BufferStats::ExperimentalV3Stats v3_stats_;

  // Duration of each bundling pass, in microseconds.
  using BundlingTimeBuckets = Histogram<10, 100, 1000, 10000, 100000>;
  BundlingTimeBuckets bundling_time_us_histogram_;

  // Counts uncompressed packet data bytes consumed by ReadNextTracePacket().
  // Used for BufferStats.bytes_read.
  uint64_t bytes_read_ = 0;

  // Backs the reference returned by stats(), which rebuilds it on every call.
  mutable TraceStats::BufferStats stats_;
};

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_V3_H_
