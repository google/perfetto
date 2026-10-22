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

#include "src/tracing/service/trace_buffer_v3.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <string.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/pb_utils.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/service/zstd_frame_codec.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace proto_utils = ::protozero::proto_utils;

namespace perfetto {

using DataLossReason = protos::pbzero::TracePacket_DataLossReason;

namespace {

using ChunkSeqReader = internal::ChunkSeqReader;
using SequenceLoss = internal::BundleBuffer::BundleMetadata::SequenceLoss;

// Create() divides the buffer capacity by this value to set
// |target_records_size_|, capped at kMaxBundleTargetSize.
constexpr size_t kBundleTargetSizeDivisor = 8;

// OnStagingNeedsSpace() divides |staging_buf_->size()| by this value to set
// |min_free_bytes|.
// Under kOverwrite, if less than |min_free_bytes| is free at the write cursor,
// wait for |min_free_bytes| new bytes before scanning |staging_buf_| again.
constexpr size_t kOverwriteSpacePassMinFreeDivisor = 8;

// ReadNextTracePacket() splits each packet into slices of at most this size,
// because the consumer IPC CHECKs that each slice fits in one IPC buffer.
// Matches TracingServiceImpl::kMaxTracePacketSliceSize.
constexpr size_t kMaxReadSliceSize = 128 * 1024 - 512;

// Appends |packet|'s record to |codec| and returns its uncompressed size.
size_t AppendBundleRecord(uint32_t sequence_index,
                          uint32_t loss_reasons,
                          const TracePacket& packet,
                          ZstdFrameCodec* codec) {
  uint8_t record_header[3 * PERFETTO_PB_VARINT_MAX_SIZE_64];
  uint8_t* header_end = proto_utils::WriteVarInt(sequence_index, record_header);
  // Keep the loss in the record header, not in the packet, because appended
  // bytes could make a truncated producer packet pass validation.
  header_end = proto_utils::WriteVarInt(loss_reasons, header_end);
  header_end = proto_utils::WriteVarInt(packet.size(), header_end);
  const size_t record_header_size =
      static_cast<size_t>(header_end - record_header);

  codec->AddToFrame(record_header, record_header_size);
  for (const Slice& slice : packet.slices())
    codec->AddToFrame(slice.start, slice.size);

  return record_header_size + packet.size();
}

// One producer packet with its sequence index and loss reasons.
// |packet_bytes| points into decompressed records held in |decode_arena_|
// for reads, or |overwritten_records_| for |protovms_| during overwrites.
struct ParsedBundleRecord {
  uint32_t sequence_index = 0;
  uint32_t loss_reasons = 0;
  protozero::ConstBytes packet_bytes{};
  size_t size = 0;  // Header and packet bytes.
};

// Reads one record at byte |offset| in the decompressed |records|.
ParsedBundleRecord ParseBundleRecord(size_t num_sequences,
                                     const uint8_t* records,
                                     size_t records_size,
                                     size_t offset) {
  const uint8_t* const begin = records + offset;
  const uint8_t* const end = records + records_size;

  uint64_t sequence_index = 0;
  const uint8_t* ptr = proto_utils::ParseVarInt(begin, end, &sequence_index);

  uint64_t loss_reasons = 0;
  ptr = proto_utils::ParseVarInt(ptr, end, &loss_reasons);

  uint64_t packet_size = 0;
  const uint8_t* next = proto_utils::ParseVarInt(ptr, end, &packet_size);

  // AppendBundleRecord() wrote every record, so a bad header or a packet that
  // runs past |records_size| means local corruption.
  PERFETTO_CHECK(next != ptr && sequence_index < num_sequences &&
                 packet_size <= static_cast<size_t>(end - next));

  ParsedBundleRecord record;
  record.sequence_index = static_cast<uint32_t>(sequence_index);
  record.loss_reasons = static_cast<uint32_t>(loss_reasons);
  record.packet_bytes = {next, static_cast<size_t>(packet_size)};
  record.size = static_cast<size_t>(next - begin) + record.packet_bytes.size;
  return record;
}

}  // namespace

namespace internal {

// +---------------------------------------------------------------------------+
// | BundleBuffer                                                              |
// +---------------------------------------------------------------------------+

bool BundleBuffer::Initialize(size_t capacity) {
  data_ = base::PagedMemory::Allocate(
      capacity, base::PagedMemory::kMayFail | base::PagedMemory::kDontCommit);
  if (PERFETTO_UNLIKELY(!data_.IsValid())) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", capacity);
    return false;
  }
  return true;
}

bool BundleBuffer::CloneFrom(const BundleBuffer& src) {
  if (PERFETTO_UNLIKELY(!Initialize(src.capacity())))
    return false;
  const size_t size = src.used_size();
  data_.EnsureCommitted(size);
  src.CopyBytes(/*offset_from_front=*/0, size, data_.Get());
  end_ = size;
  max_written_offset_ = size;
  return true;
}

// static
uint64_t BundleBuffer::GetMetadataSize(uint32_t num_sequences,
                                       uint32_t num_losses) {
  // Push() and PeekFrontMetadata() copy these as raw bytes, so they must be
  // trivially copyable and have no padding.
  static_assert(std::is_trivially_copyable_v<BundleHeader>);
  static_assert(std::is_trivially_copyable_v<SequenceLoss>);
  static_assert(sizeof(BundleHeader) == 16);
  static_assert(sizeof(SequenceLoss) == 8);

  return sizeof(BundleHeader) +
         uint64_t{num_sequences} * sizeof(ProducerAndWriterID) +
         uint64_t{num_losses} * sizeof(SequenceLoss);
}

// static
uint64_t BundleBuffer::GetBundleSize(size_t num_sequences,
                                     size_t num_losses,
                                     size_t compressed_records_size) {
  if (PERFETTO_UNLIKELY(num_sequences > std::numeric_limits<uint32_t>::max() ||
                        num_losses > std::numeric_limits<uint32_t>::max() ||
                        compressed_records_size >
                            std::numeric_limits<uint32_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }

  return GetMetadataSize(static_cast<uint32_t>(num_sequences),
                         static_cast<uint32_t>(num_losses)) +
         compressed_records_size;
}

void BundleBuffer::Push(const BundleMetadata& metadata,
                        protozero::ConstBytes compressed_records) {
  const auto& sequence_keys = metadata.sequence_keys;
  const auto& sequence_losses = metadata.sequence_losses;
  const uint64_t bundle_size = GetBundleSize(
      sequence_keys.size(), sequence_losses.size(), compressed_records.size);
  PERFETTO_DCHECK(bundle_size <= free_size());
  BundleHeader header{};
  header.bundle_size = static_cast<uint32_t>(bundle_size);
  header.num_sequences = static_cast<uint32_t>(sequence_keys.size());
  header.num_losses = static_cast<uint32_t>(sequence_losses.size());
  header.records_size = metadata.records_size;
  AppendBytes(&header, sizeof(header));
  AppendBytes(sequence_keys.data(),
              sequence_keys.size() * sizeof(ProducerAndWriterID));
  AppendBytes(sequence_losses.data(),
              sequence_losses.size() * sizeof(SequenceLoss));
  AppendBytes(compressed_records.data, compressed_records.size);
}

void BundleBuffer::PeekFrontMetadata(BundleMetadata* metadata) const {
  const BundleHeader header = PeekFrontHeader();
  const size_t keys_size = header.num_sequences * sizeof(ProducerAndWriterID);
  metadata->sequence_keys.resize(header.num_sequences);
  CopyBytes(sizeof(header), keys_size, metadata->sequence_keys.data());
  metadata->sequence_losses.resize(header.num_losses);
  CopyBytes(sizeof(header) + keys_size,
            header.num_losses * sizeof(SequenceLoss),
            metadata->sequence_losses.data());
  metadata->records_size = header.records_size;
}

Slice BundleBuffer::PeekFrontCompressedRecords() const {
  const BundleHeader header = PeekFrontHeader();
  const size_t metadata_size = static_cast<size_t>(
      GetMetadataSize(header.num_sequences, header.num_losses));
  const size_t compressed_records_size = header.bundle_size - metadata_size;
  const size_t mem_offset = OffsetOf(begin_ + metadata_size);

  // zstd needs the compressed records in one piece, so if they wrap around the
  // end of |data_|, copy both parts into a new allocation.
  if (PERFETTO_UNLIKELY(compressed_records_size > data_.size() - mem_offset)) {
    Slice compressed_records = Slice::Allocate(compressed_records_size);
    CopyBytes(metadata_size, compressed_records_size,
              compressed_records.own_data());
    return compressed_records;
  }

  return {static_cast<const uint8_t*>(data_.Get()) + mem_offset,
          compressed_records_size};
}

void BundleBuffer::Pop() {
  begin_ += PeekFrontHeader().bundle_size;
  if (begin_ == end_) {
    begin_ = 0;
    end_ = 0;
  }
}

void BundleBuffer::MaybeCompact() {
  if (!empty())
    return;

  // TODO(sashwinbalaji): Move this spare-page policy, copied from
  // TraceBufferV2::CompactChunks(), into a helper that both share.
  constexpr size_t kSpareCapacityDivisor = 4;
  const size_t spare_size =
      std::min(max_written_offset_ / kSpareCapacityDivisor,
               data_.size() - max_written_offset_);
  const size_t cur_madvise_begin = max_written_offset_ + spare_size;

  // Writes since the last compaction touched pages up to |max_written_offset_|.
  prev_madvise_begin_ = std::max(prev_madvise_begin_, max_written_offset_);

  // Pages at or past |prev_madvise_begin_| are already released or were never
  // written, so skip a madvise that starts there.
  if (cur_madvise_begin < prev_madvise_begin_) {
    data_.AdviseDontNeedAfter(cur_madvise_begin);
    prev_madvise_begin_ = cur_madvise_begin;
  }
  max_written_offset_ = 0;
}

void BundleBuffer::AppendBytes(const void* src, size_t size) {
  if (PERFETTO_UNLIKELY(size == 0))
    return;
  const size_t mem_offset = OffsetOf(end_);
  const size_t size_to_end = std::min(size, data_.size() - mem_offset);
  data_.EnsureCommitted(mem_offset + size_to_end);

  uint8_t* mem_begin = static_cast<uint8_t*>(data_.Get());
  memcpy(mem_begin + mem_offset, src, size_to_end);

  // If the write wraps, copy the remaining bytes to the start of |data_|.
  if (PERFETTO_UNLIKELY(size > size_to_end)) {
    memcpy(mem_begin, static_cast<const uint8_t*>(src) + size_to_end,
           size - size_to_end);
    max_written_offset_ = data_.size();
  } else {
    max_written_offset_ = std::max(max_written_offset_, mem_offset + size);
  }
  end_ += size;
}

BundleBuffer::BundleHeader BundleBuffer::PeekFrontHeader() const {
  BundleHeader header{};
  CopyBytes(/*offset_from_front=*/0, sizeof(header), &header);

  // Push() writes these sizes, so invalid values mean local corruption.
  // Fail here because we cannot find the next bundle without them.
  PERFETTO_CHECK(GetMetadataSize(header.num_sequences, header.num_losses) <=
                     header.bundle_size &&
                 header.bundle_size <= used_size());
  return header;
}

void BundleBuffer::CopyBytes(size_t offset_from_front,
                             size_t size,
                             void* dst) const {
  PERFETTO_DCHECK(offset_from_front <= used_size() &&
                  size <= used_size() - offset_from_front);
  if (PERFETTO_UNLIKELY(size == 0))
    return;
  const size_t mem_offset = OffsetOf(begin_ + offset_from_front);
  const size_t size_to_end = std::min(size, data_.size() - mem_offset);
  const uint8_t* mem_begin = static_cast<const uint8_t*>(data_.Get());
  memcpy(dst, mem_begin + mem_offset, size_to_end);

  // If the read wraps, copy the remaining bytes from the start of |data_|.
  if (PERFETTO_UNLIKELY(size > size_to_end)) {
    memcpy(static_cast<uint8_t*>(dst) + size_to_end, mem_begin,
           size - size_to_end);
  }
}

size_t BundleBuffer::OffsetOf(uint64_t position) const {
  return static_cast<size_t>(position % data_.size());
}

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+

DecodeArena::DecodeArena(size_t block_size) : block_size_(block_size) {}

uint8_t* DecodeArena::Allocate(size_t size) {
  Allocation* block = nullptr;
  for (Allocation& candidate : blocks_) {
    if (size <= block_size_ - candidate.used_size) {
      block = &candidate;
      break;
    }
  }

  if (PERFETTO_UNLIKELY(!block)) {
    temporaries_.push_back(
        {std::unique_ptr<uint8_t[]>(new uint8_t[size]), size});
    return temporaries_.back().data.get();
  }

  if (!block->data)
    block->data.reset(new uint8_t[block_size_]);

  uint8_t* records_begin = block->data.get() + block->used_size;
  block->used_size += size;
  return records_begin;
}

void DecodeArena::ResetExcept(const uint8_t* live_records) {
  // No bundle is live, so reuse |blocks_| and free |temporaries_|.
  if (!live_records) {
    for (Allocation& block : blocks_)
      block.used_size = 0;
    temporaries_ = std::vector<Allocation>();
    return;
  }

  const uintptr_t live_records_addr = reinterpret_cast<uintptr_t>(live_records);
  for (Allocation& block : blocks_) {
    if (!block.data)
      continue;

    const uintptr_t block_begin_addr =
        reinterpret_cast<uintptr_t>(block.data.get());
    if (block_begin_addr <= live_records_addr &&
        live_records_addr < block_begin_addr + block_size_) {
      // Keep the block that holds the live records.
      continue;
    }
    block.used_size = 0;
  }

  if (PERFETTO_UNLIKELY(!temporaries_.empty())) {
    // A temporary holds one bundle, so compare |live_records| with its start.
    std::vector<Allocation> live_temporaries;
    for (Allocation& temporary : temporaries_) {
      if (temporary.data.get() == live_records) {
        live_temporaries.push_back(std::move(temporary));
        break;
      }
    }
    temporaries_ = std::move(live_temporaries);
  }
}

void DecodeArena::CompactExcept(const uint8_t* live_records) {
  ResetExcept(live_records);
  for (Allocation& block : blocks_) {
    if (block.used_size == 0)
      block.data.reset();
  }
}

size_t DecodeArena::GetMemoryUsageBytes() const {
  size_t total_bytes = blocks_.size() * block_size_;
  for (const Allocation& temporary : temporaries_)
    total_bytes += temporary.used_size;
  return total_bytes;
}

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::Create(
    size_t size_in_bytes,
    OverwritePolicy policy,
    std::shared_ptr<ZstdFrameCodec> codec) {
  const size_t capacity =
      base::AlignUp(std::max(size_in_bytes, size_t(1)), 4096);
  return CreateInternal(
      capacity, policy,
      std::min(capacity / kBundleTargetSizeDivisor, kMaxBundleTargetSize),
      std::move(codec));
}

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::CreateInternal(
    size_t size_in_bytes,
    OverwritePolicy policy,
    size_t target_records_size,
    std::shared_ptr<ZstdFrameCodec> codec) {
  const size_t capacity = std::max(size_in_bytes, size_t(1));
  std::unique_ptr<TraceBufferV3> buf(
      new TraceBufferV3(target_records_size, std::move(codec)));
  // TraceBufferV2::Create() CHECKs that |capacity| fits in 32 bits.
  // |bundle_buf_| shares this capacity, so stored bundle sizes fit too.
  buf->staging_buf_ = TraceBufferV2::Create(capacity, policy);
  if (PERFETTO_UNLIKELY(!buf->staging_buf_ ||
                        !buf->bundle_buf_.Initialize(capacity))) {
    return nullptr;
  }
  return buf;
}

TraceBufferV3::TraceBufferV3(size_t target_records_size,
                             std::shared_ptr<ZstdFrameCodec> codec)
    : target_records_size_(target_records_size),
      bytes_until_checkpoint_pass_(target_records_size),
      codec_(std::move(codec)),
      // A bundle can exceed |target_records_size| by its final record's size.
      // Twice the target fits it when that record is no larger than the target.
      decode_arena_(std::max(kMaxBundleTargetSize, 2 * target_records_size)) {}

TraceBufferV3::~TraceBufferV3() = default;

void TraceBufferV3::CopyChunkUntrusted(
    ProducerID producer_id_trusted,
    const ClientIdentity& client_identity_trusted,
    WriterID writer_id,
    ChunkID chunk_id,
    uint16_t num_fragments,
    uint8_t chunk_flags,
    bool chunk_complete,
    const uint8_t* src,
    size_t size) {
  auto chunk = staging_buf_->PrepareChunk(
      producer_id_trusted, client_identity_trusted, writer_id, chunk_id,
      num_fragments, chunk_flags, chunk_complete, src, size);
  if (PERFETTO_UNLIKELY(!chunk))
    return;

  const uint64_t bytes_written_before = staging_buf_->stats().bytes_written();

  const auto result = staging_buf_->TryCopyChunk(
      *chunk, TraceBufferV2::WriteMode::kAvoidOverwrite);
  if (PERFETTO_UNLIKELY(result == TraceBufferV2::WriteResult::kNeedsSpace)) {
    // The write would wrap or overwrite |staging_buf_|, so try to bundle first.
    OnStagingNeedsSpace();
    // Under kOverwrite, the retry may overwrite chunks that remain unread.
    staging_buf_->TryCopyChunk(*chunk,
                               TraceBufferV2::WriteMode::kAllowOverwrite);
  }

  OnStagingChunkCopyComplete(bytes_written_before);
}

bool TraceBufferV3::TryPatchChunkContents(ProducerID producer_id,
                                          WriterID writer_id,
                                          ChunkID chunk_id,
                                          const Patch* patches,
                                          size_t patches_size,
                                          bool other_patches_pending) {
  return staging_buf_->TryPatchChunkContents(producer_id, writer_id, chunk_id,
                                             patches, patches_size,
                                             other_patches_pending);
}

bool TraceBufferV3::supports_tracing_v2() const {
  return true;
}

bool TraceBufferV3::CopyChunkV2Untrusted(
    const PacketSequenceProperties& sequence,
    const protozero::ConstBytes* fragments,
    size_t num_fragments,
    bool first_continues_from_prev,
    bool last_continues_on_next) {
  auto chunk = staging_buf_->PrepareChunkV2(sequence, fragments, num_fragments,
                                            first_continues_from_prev,
                                            last_continues_on_next);
  if (PERFETTO_UNLIKELY(!chunk))
    return false;

  const uint64_t bytes_written_before = staging_buf_->stats().bytes_written();

  auto result = staging_buf_->TryCopyChunkV2(
      *chunk, TraceBufferV2::WriteMode::kAvoidOverwrite);
  if (PERFETTO_UNLIKELY(result == TraceBufferV2::WriteResult::kNeedsSpace)) {
    OnStagingNeedsSpace();
    result = staging_buf_->TryCopyChunkV2(
        *chunk, TraceBufferV2::WriteMode::kAllowOverwrite);
  }

  OnStagingChunkCopyComplete(bytes_written_before);
  return result == TraceBufferV2::WriteResult::kAccepted;
}

void TraceBufferV3::RecordChunkV2DataLoss(ProducerID producer_id,
                                          WriterID writer_id) {
  staging_buf_->RecordChunkV2DataLoss(producer_id, writer_id);
}

void TraceBufferV3::RecordAbiViolation() {
  staging_buf_->RecordAbiViolation();
}

void TraceBufferV3::OnStagingNeedsSpace() {
  auto run_space_pass = [this] {
    // SMB v1 scraping can copy chunks out of ChunkID order.
    // Do not skip a missing chunk that may arrive later in this write cycle.
    BundleStagingForWrite(ChunkSeqReader::kWaitForMissingChunksMode);
    v3_stats_.set_staging_space_passes(v3_stats_.staging_space_passes() + 1);
  };

  if (overwrite_policy() == kDiscard) {
    // kDiscard stops writes if the incoming chunk still cannot fit.
    // Try bundling readable packets to free space in |staging_buf_| first.
    run_space_pass();
    return;
  }

  // Let the caller overwrite old chunks in |staging_buf_| without scanning it.
  // Those writes reduce this counter before we try bundling packets again.
  if (overwrite_bytes_until_space_pass_ > 0)
    return;

  run_space_pass();

  // Packets waiting for fragments or patches may keep |staging_buf_| full.
  // Wait for more writes to avoid repeating this scan for each small chunk.
  const size_t min_free_bytes =
      staging_buf_->size() / kOverwriteSpacePassMinFreeDivisor;
  if (staging_buf_->WriteWouldWrapOrOverwrite(min_free_bytes)) {
    overwrite_bytes_until_space_pass_ = min_free_bytes;
  }
}

void TraceBufferV3::OnStagingChunkCopyComplete(uint64_t bytes_written_before) {
  const uint64_t bytes_written =
      staging_buf_->stats().bytes_written() - bytes_written_before;
  bytes_until_checkpoint_pass_ -= static_cast<size_t>(
      std::min<uint64_t>(bytes_written, bytes_until_checkpoint_pass_));

  if (overwrite_policy() == kOverwrite) {
    overwrite_bytes_until_space_pass_ -= static_cast<size_t>(
        std::min<uint64_t>(bytes_written, overwrite_bytes_until_space_pass_));
  } else if (PERFETTO_UNLIKELY(staging_buf_->discard_writes())) {
    // Set |discard_writes_| too, so later writes and OnWriteCheckpoint()
    // cannot start more bundling after |staging_buf_| has stopped writes.
    StopAcceptingWrites();
  }
}

void TraceBufferV3::StopAcceptingWrites() {
  PERFETTO_DCHECK(overwrite_policy() == kDiscard);
  discard_writes_ = true;
  staging_buf_->StopAcceptingWrites();
}

void TraceBufferV3::OnWriteCheckpoint() {
  if (PERFETTO_UNLIKELY(discard_writes_))
    return;

  if (bytes_until_checkpoint_pass_ > 0)
    return;

  // Restart the count even if the pass reads nothing, so blocked sequences
  // cannot trigger a pass on every checkpoint.
  bytes_until_checkpoint_pass_ = target_records_size_;

  // The write cycle is done, so we can read past any remaining gaps.
  BundleStagingForWrite(ChunkSeqReader::kReadMode);
}

void TraceBufferV3::BundleStagingForWrite(ChunkSeqReader::Mode read_mode) {
  if (PERFETTO_UNLIKELY(discard_writes_))
    return;

  const base::TimeNanos start = base::GetWallTimeNs();
  staging_buf_->BeginRead(read_mode);

  // Under kDiscard, stop this pass once cumulative stored bytes reach
  // |bundle_buf_|'s capacity or a bundle must wait in |overflow_bundle_|.
  while (!discard_writes_) {
    const BundleResult result = BuildNextBundle();
    if (result == BundleResult::kStagingReadComplete) {
      // No more packets are readable, so CompactChunks() can reset the cursor.
      staging_buf_->CompactChunks();
      break;
    }
  }

  RecordBundlingTime(start);
}

bool TraceBufferV3::BundleStagingForRead() {
  PERFETTO_DCHECK(bundle_buf_.empty() && !overflow_bundle_);
  if (staging_read_complete_)
    return false;

  const base::TimeNanos start = base::GetWallTimeNs();

  // Empty packets and oversized bundles can leave |bundle_buf_| empty.
  // Stop once a bundle is stored or |staging_buf_| has no readable packets.
  while (bundle_buf_.empty()) {
    const BundleResult result = BuildNextBundle();
    if (result == BundleResult::kStagingReadComplete) {
      staging_read_complete_ = true;
      // No more packets are readable from |staging_buf_|, so make
      // OnWriteCheckpoint() wait for new bytes before bundling again.
      bytes_until_checkpoint_pass_ = target_records_size_;
      staging_buf_->CompactChunks();
      break;
    }
  }
  RecordBundlingTime(start);
  return !bundle_buf_.empty();
}

void TraceBufferV3::RecordBundlingTime(base::TimeNanos start) {
  const uint64_t duration_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          base::GetWallTimeNs() - start)
          .count());
  v3_stats_.set_bundling_max_us(
      std::max(v3_stats_.bundling_max_us(), duration_us));
  bundling_time_us_histogram_.Add(static_cast<HistValue>(duration_us));
}

TraceBufferV3::BundleResult TraceBufferV3::BuildNextBundle() {
  PERFETTO_DCHECK(open_bundle_.metadata.sequence_keys.empty());

  // |staging_buf_| reserves sequence key 0 for padding.
  ProducerAndWriterID last_sequence_key = 0;
  uint32_t sequence_index = 0;
  BundleResult result = BundleResult::kTargetReached;

  for (;;) {
    staged_packet_.Clear();
    PacketSequenceProperties properties{};
    uint32_t loss_reasons = 0;
    if (!staging_buf_->ReadNextTracePacket(&staged_packet_, &properties,
                                           &loss_reasons)) {
      result = BundleResult::kStagingReadComplete;
      break;
    }

    const ProducerAndWriterID sequence_key = MkProducerAndWriterID(
        properties.producer_id_trusted, properties.writer_id);

    // SMB v1 can reassemble an empty packet from empty fragments.
    // Keep its loss for the next packet on this sequence that leaves staging.
    if (PERFETTO_UNLIKELY(staged_packet_.size() == 0)) {
      if (loss_reasons)
        staging_buf_->RecordSequenceDataLoss(sequence_key, loss_reasons);
      continue;
    }

    // Cache the last |sequence_index| to avoid a lookup for consecutive packets
    // on the same sequence.
    if (sequence_key != last_sequence_key) {
      auto& sequence_keys = open_bundle_.metadata.sequence_keys;
      // The first record opens the frame, so a bundle without records leaves
      // no frame open on the shared |codec_|.
      if (sequence_keys.empty())
        codec_->BeginFrame();
      auto [index, inserted] = open_bundle_.sequence_indexes.Insert(
          sequence_key, static_cast<uint32_t>(sequence_keys.size()));
      if (inserted)
        sequence_keys.push_back(sequence_key);
      sequence_index = *index;
      last_sequence_key = sequence_key;
    }

    // TODO(sashwinbalaji): Include the packet's previous_packet_dropped bits
    // (e.g. SMB_FULL) in |sequence_losses| so dropping a bundle preserves them.
    // The service also replaces these bits when it appends its own loss field.
    // TraceBufferV2 has the same issue.
    if (PERFETTO_UNLIKELY(loss_reasons)) {
      // Keep one entry per sequence.
      // A sequence's packets often come in runs, so search from the newest.
      auto& sequence_losses = open_bundle_.metadata.sequence_losses;
      auto loss = std::find_if(sequence_losses.rbegin(), sequence_losses.rend(),
                               [sequence_key](const SequenceLoss& entry) {
                                 return entry.sequence_key == sequence_key;
                               });
      if (loss != sequence_losses.rend()) {
        loss->loss_reasons |= loss_reasons;
      } else {
        sequence_losses.push_back({sequence_key, loss_reasons});
      }
    }
    open_bundle_.records_size += AppendBundleRecord(
        sequence_index, loss_reasons, staged_packet_, codec_.get());

    // Keep the packet that crosses the target whole.
    if (open_bundle_.records_size >= target_records_size_)
      break;
  }  // for (;;)

  // |codec_| has copied or compressed the slices, so release |staged_packet_|
  // before compaction can move |staging_buf_|'s memory.
  staged_packet_.Clear();
  CloseOpenBundle();
  return result;
}

void TraceBufferV3::CloseOpenBundle() {
  auto reset_open_bundle = base::OnScopeExit([this] {
    open_bundle_.metadata.sequence_keys.clear();
    open_bundle_.metadata.sequence_losses.clear();
    open_bundle_.metadata.records_size = 0;
    open_bundle_.sequence_indexes.Clear();
    open_bundle_.records_size = 0;
  });

  BundleMetadata& metadata = open_bundle_.metadata;

  // We may find no readable packets, or only empty packets that we skip, so
  // no frame is open.
  if (metadata.sequence_keys.empty())
    return;

  const protozero::ConstBytes compressed_records = codec_->EndFrame();
  const uint64_t bundle_size = internal::BundleBuffer::GetBundleSize(
      metadata.sequence_keys.size(), metadata.sequence_losses.size(),
      compressed_records.size);

  // Drop bundles that exceed |bundle_buf_|'s capacity or whose uncompressed
  // records exceed UINT32_MAX bytes.
  // Later packets are still in |staging_buf_|, so record the loss there.
  if (PERFETTO_UNLIKELY(bundle_size > bundle_buf_.capacity() ||
                        open_bundle_.records_size >
                            std::numeric_limits<uint32_t>::max())) {
    for (const SequenceLoss& loss : metadata.sequence_losses) {
      staging_buf_->RecordSequenceDataLoss(loss.sequence_key,
                                           loss.loss_reasons);
    }
    for (ProducerAndWriterID sequence_key : metadata.sequence_keys) {
      staging_buf_->RecordSequenceDataLoss(sequence_key,
                                           DataLossReason::DATA_LOSS_PRESENT);
    }
    v3_stats_.set_bundles_too_large(v3_stats_.bundles_too_large() + 1);
    return;
  }

  // Count this bundle in each sequence's |bundle_ref_count| before overwrites.
  // Otherwise the count could reach zero and move loss to |staging_buf_|,
  // skipping the packets already stored in this new bundle.
  for (ProducerAndWriterID sequence_key : metadata.sequence_keys)
    AddSequenceRef(sequence_key);

  metadata.records_size = static_cast<uint32_t>(open_bundle_.records_size);
  v3_stats_.set_bundles_written(v3_stats_.bundles_written() + 1);
  v3_stats_.set_bundle_bytes_uncompressed(
      v3_stats_.bundle_bytes_uncompressed() + metadata.records_size);
  v3_stats_.set_bundle_bytes_stored(v3_stats_.bundle_bytes_stored() +
                                    bundle_size);

  if (overwrite_policy() == kOverwrite) {
    while (bundle_size > bundle_buf_.free_size())
      OverwriteFrontBundle();
    bundle_buf_.Push(metadata, compressed_records);
    return;
  }

  if (PERFETTO_UNLIKELY(bundle_size > bundle_buf_.free_size())) {
    // The bundle doesn't fit in the free space, so stop accepting writes.
    StopAcceptingWrites();

    // These packets have left |staging_buf_|, so keep them in
    // |overflow_bundle_| until the consumer reads the older bundles.
    PERFETTO_DCHECK(!overflow_bundle_);
    overflow_bundle_.emplace();
    overflow_bundle_->metadata = metadata;
    overflow_bundle_->compressed_records.assign(
        compressed_records.data,
        compressed_records.data + compressed_records.size);
    return;
  }
  // With kDiscard, stop accepting writes once we've stored a buffer's worth
  // of bundles, even if reads have freed some space.
  if (PERFETTO_UNLIKELY(!discard_writes_ && v3_stats_.bundle_bytes_stored() >=
                                                bundle_buf_.capacity())) {
    StopAcceptingWrites();
  }

  bundle_buf_.Push(metadata, compressed_records);
}

void TraceBufferV3::AddSequenceRef(ProducerAndWriterID sequence_key) {
  auto [bundled_sequence, inserted] =
      bundled_sequences_.Insert(sequence_key, {});
  if (inserted)
    staging_buf_->PreventSequencePruning(sequence_key);
  ++bundled_sequence->bundle_ref_count;
}

void TraceBufferV3::ReleaseSequenceRef(ProducerAndWriterID sequence_key,
                                       uint32_t loss_reasons) {
  // Each bundle took a reference on every sequence in its list.
  auto* bundled_sequence = bundled_sequences_.Find(sequence_key);
  PERFETTO_CHECK(bundled_sequence);

  bundled_sequence->unreported_loss_reasons |= loss_reasons;
  --bundled_sequence->bundle_ref_count;

  if (bundled_sequence->bundle_ref_count > 0)
    return;

  // The next packet on this sequence now comes from |staging_buf_|.
  // Return its unreported loss and let staging decide when to prune its state.
  if (bundled_sequence->unreported_loss_reasons) {
    staging_buf_->RecordSequenceDataLoss(
        sequence_key, bundled_sequence->unreported_loss_reasons);
  }
  staging_buf_->AllowSequencePruning(sequence_key);
  bundled_sequences_.Erase(sequence_key);
}

void TraceBufferV3::OverwriteFrontBundle() {
  PERFETTO_DCHECK(overwrite_policy() == kOverwrite);
  bundle_buf_.PeekFrontMetadata(&front_bundle_metadata_);
  const BundleMetadata& metadata = front_bundle_metadata_;
  const auto& sequence_keys = metadata.sequence_keys;

  v3_stats_.set_bundles_overwritten(v3_stats_.bundles_overwritten() + 1);

  // Decode only if a producer in the bundle has a ProtoVM.
  const bool any_producer_has_protovm =
      !protovms_.empty() &&
      std::any_of(sequence_keys.begin(), sequence_keys.end(),
                  [this](ProducerAndWriterID sequence_key) {
                    ProducerID producer_id = 0;
                    WriterID writer_id = 0;
                    GetProducerAndWriterID(sequence_key, &producer_id,
                                           &writer_id);
                    return protovms_.HasProducer(producer_id);
                  });
  if (any_producer_has_protovm) {
    if (metadata.records_size > overwritten_records_capacity_) {
      overwritten_records_.reset(new uint8_t[metadata.records_size]);
      overwritten_records_capacity_ = metadata.records_size;
    }
    uint8_t* records = overwritten_records_.get();
    DecompressFrontBundle(records, metadata.records_size);

    uint32_t last_sequence_index = std::numeric_limits<uint32_t>::max();
    ProducerID producer_id = 0;
    bool producer_has_protovm = false;
    TracePacket packet;
    for (size_t offset = 0; offset < metadata.records_size;) {
      const ParsedBundleRecord record = ParseBundleRecord(
          sequence_keys.size(), records, metadata.records_size, offset);
      offset += record.size;

      if (record.sequence_index != last_sequence_index) {
        last_sequence_index = record.sequence_index;
        WriterID writer_id = 0;
        GetProducerAndWriterID(sequence_keys[last_sequence_index], &producer_id,
                               &writer_id);
        producer_has_protovm = protovms_.HasProducer(producer_id);
      }
      if (!producer_has_protovm)
        continue;

      packet.Clear();
      packet.AddSlice(record.packet_bytes.data, record.packet_bytes.size);
      protovms_.MaybeProcessOverwrittenPacket(packet, producer_id);
    }
  }  // if (any_producer_has_protovm)

  // Preserve the overwritten packets' earlier loss before releasing references.
  // A sequence's last release moves its loss to |staging_buf_| and removes its
  // |bundled_sequences_| entry.
  for (const SequenceLoss& loss : metadata.sequence_losses) {
    auto* bundled_sequence = bundled_sequences_.Find(loss.sequence_key);
    PERFETTO_CHECK(bundled_sequence);
    bundled_sequence->unreported_loss_reasons |= loss.loss_reasons;
  }
  for (ProducerAndWriterID sequence_key : sequence_keys) {
    ReleaseSequenceRef(sequence_key, DataLossReason::DATA_LOSS_PRESENT |
                                         DataLossReason::DATA_LOSS_OVERWRITE);
  }

  bundle_buf_.Pop();
}

void TraceBufferV3::DecompressFrontBundle(uint8_t* dst, size_t dst_size) {
  const auto compressed_records = bundle_buf_.PeekFrontCompressedRecords();
  const bool decompressed = codec_->Decompress(
      /*frame=*/{static_cast<const uint8_t*>(compressed_records.start),
                 compressed_records.size},
      dst, dst_size);
  PERFETTO_CHECK(decompressed);
}

void TraceBufferV3::BeginRead() {
  decode_arena_.ResetExcept(decoded_bundle_.records);
  staging_buf_->BeginRead();
  staging_read_complete_ = false;
}

bool TraceBufferV3::ReadNextTracePacket(
    TracePacket* packet,
    PacketSequenceProperties* sequence_properties,
    uint32_t* previous_packet_on_sequence_dropped) {
  *sequence_properties = {/*producer_id_trusted=*/0,
                          /*client_identity_trusted=*/ClientIdentity(),
                          /*writer_id=*/0};
  *previous_packet_on_sequence_dropped = 0;

  if (!decoded_bundle_.records && !DecodeNextBundle())
    return false;

  const ParsedBundleRecord record = ParseBundleRecord(
      decoded_bundle_.sequences.size(), decoded_bundle_.records,
      decoded_bundle_.records_size, decoded_bundle_.read_offset);
  decoded_bundle_.read_offset += record.size;
  auto& sequence = decoded_bundle_.sequences[record.sequence_index];
  for (size_t offset = 0; offset < record.packet_bytes.size;
       offset += kMaxReadSliceSize) {
    packet->AddSlice(
        record.packet_bytes.data + offset,
        std::min(kMaxReadSliceSize, record.packet_bytes.size - offset));
  }

  *sequence_properties = sequence.properties;
  *previous_packet_on_sequence_dropped =
      sequence.unreported_loss_reasons | record.loss_reasons;
  sequence.unreported_loss_reasons = 0;
  bytes_read_ += record.size;

  // After the last packet, release the bundle's sequence references.
  // The records stay in |decode_arena_|, so returned slices stay valid.
  if (decoded_bundle_.read_offset == decoded_bundle_.records_size) {
    for (const auto& decoded_sequence : decoded_bundle_.sequences) {
      // All packets were returned, so releasing this bundle adds no loss.
      ReleaseSequenceRef(decoded_sequence.sequence_key, /*loss_reasons=*/0);
    }
    decoded_bundle_.sequences.clear();
    decoded_bundle_.records = nullptr;
    decoded_bundle_.records_size = 0;
    decoded_bundle_.read_offset = 0;
  }
  return true;
}

bool TraceBufferV3::DecodeNextBundle() {
  // Drain |bundle_buf_| and |overflow_bundle_| before reading |staging_buf_|
  // so bundling cannot overwrite older packets still waiting to be read.
  if (bundle_buf_.empty()) {
    if (overflow_bundle_) {
      // Older bundles are gone, and this bundle fits the empty ring buffer, so
      // move it there and decode it like a stored bundle.
      bundle_buf_.Push(overflow_bundle_->metadata,
                       {overflow_bundle_->compressed_records.data(),
                        overflow_bundle_->compressed_records.size()});
      overflow_bundle_.reset();
    } else if (!BundleStagingForRead()) {
      return false;
    }
  }

  bundle_buf_.PeekFrontMetadata(&front_bundle_metadata_);
  const BundleMetadata& metadata = front_bundle_metadata_;
  const uint32_t records_size = metadata.records_size;

  uint8_t* records = decode_arena_.Allocate(records_size);
  DecompressFrontBundle(records, records_size);

  decoded_bundle_.sequences.reserve(metadata.sequence_keys.size());
  for (ProducerAndWriterID sequence_key : metadata.sequence_keys) {
    DecodedBundle::Sequence sequence{};
    sequence.sequence_key = sequence_key;
    GetProducerAndWriterID(sequence_key,
                           &sequence.properties.producer_id_trusted,
                           &sequence.properties.writer_id);
    sequence.properties.client_identity_trusted =
        staging_buf_->GetSequenceClientIdentity(sequence_key);
    // Save the loss preceding this bundle in |sequence| for its first packet.
    // Writes may overwrite newer bundles while this one is partly read, so
    // those later losses must not be reported on this bundle's earlier packets.
    auto* bundled_sequence = bundled_sequences_.Find(sequence_key);
    PERFETTO_CHECK(bundled_sequence);
    sequence.unreported_loss_reasons =
        bundled_sequence->unreported_loss_reasons;
    bundled_sequence->unreported_loss_reasons = 0;
    decoded_bundle_.sequences.push_back(sequence);
  }
  decoded_bundle_.records = records;
  decoded_bundle_.records_size = records_size;

  // Reads now use the records in |decode_arena_|, so reclaim the compressed
  // bundle's space in |bundle_buf_| even while its packets remain unread.
  bundle_buf_.Pop();
  return true;
}

void TraceBufferV3::MaybeCompact() {
  decode_arena_.CompactExcept(decoded_bundle_.records);
  overwritten_records_.reset();
  overwritten_records_capacity_ = 0;
  // kDiscard counts total bundle bytes in |v3_stats_.bundle_bytes_stored()|.
  // Compacting |staging_buf_| does not reset that count, so writes still stop
  // once the total reaches |bundle_buf_|'s capacity.
  staging_buf_->CompactChunks();
  bundle_buf_.MaybeCompact();
}

std::unique_ptr<TraceBuffer> TraceBufferV3::CloneReadOnly() const {
  std::unique_ptr<TraceBufferV3> clone(
      new TraceBufferV3(target_records_size_, codec_));
  clone->staging_buf_.reset(
      static_cast<TraceBufferV2*>(staging_buf_->CloneReadOnly().release()));
  if (PERFETTO_UNLIKELY(!clone->staging_buf_))
    return nullptr;
  if (PERFETTO_UNLIKELY(!clone->bundle_buf_.CloneFrom(bundle_buf_)))
    return nullptr;

  clone->discard_writes_ = discard_writes_;
  clone->overflow_bundle_ = overflow_bundle_;
  for (auto it = bundled_sequences_.GetIterator(); it; ++it)
    clone->bundled_sequences_.Insert(it.key(), it.value());
  clone->protovms_.CloneReadOnlyFrom(protovms_);
  // Keep bundling statistics, but start |bytes_read_| at zero for the clone.
  clone->v3_stats_ = v3_stats_;
  clone->bundling_time_us_histogram_ = bundling_time_us_histogram_;

  if (!decoded_bundle_.records)
    return clone;

  clone->decoded_bundle_ = decoded_bundle_;
  uint8_t* records =
      clone->decode_arena_.Allocate(decoded_bundle_.records_size);
  memcpy(records, decoded_bundle_.records, decoded_bundle_.records_size);
  clone->decoded_bundle_.records = records;
  return clone;
}

TraceBufferProtoVms* TraceBufferV3::protovms() {
  return &protovms_;
}

void TraceBufferV3::set_read_only() {
  staging_buf_->set_read_only();
}

const TraceStats::BufferStats& TraceBufferV3::stats() const {
  stats_ = staging_buf_->stats();
  stats_.set_buffer_size(bundle_buf_.capacity());

  // Report bytes consumed by ReadNextTracePacket(), including record headers.
  // Exclude |staging_buf_|'s read counts because its reads only build bundles.
  stats_.set_bytes_read(bytes_read_);
  stats_.set_chunks_read(0);
  stats_.set_readaheads_succeeded(0);
  stats_.set_readaheads_failed(0);

  auto* v3_stats = stats_.mutable_experimental_v3_stats();
  *v3_stats = v3_stats_;
  const auto& hist = bundling_time_us_histogram_;
  for (size_t i = 0; i < hist.num_buckets(); ++i) {
    if (i + 1 < hist.num_buckets())
      v3_stats->add_bundling_time_us_histogram_def(hist.GetBucketThres(i));
    v3_stats->add_bundling_time_us_histogram_counts(hist.GetBucketCount(i));
    v3_stats->add_bundling_time_us_histogram_sum(hist.GetBucketSum(i));
  }
  return stats_;
}

const TraceBuffer::WriterStats& TraceBufferV3::writer_stats() const {
  return staging_buf_->writer_stats();
}

size_t TraceBufferV3::size() const {
  return bundle_buf_.capacity();
}

size_t TraceBufferV3::used_size() const {
  return bundle_buf_.used_size();
}

size_t TraceBufferV3::GetMemoryUsageBytes() const {
  size_t total_bytes = bundle_buf_.capacity();
  total_bytes += staging_buf_->GetMemoryUsageBytes();
  total_bytes += protovms_.GetMemoryUsageBytes();
  total_bytes += decode_arena_.GetMemoryUsageBytes();
  total_bytes += overwritten_records_capacity_;
  if (overflow_bundle_) {
    const BundleMetadata& metadata = overflow_bundle_->metadata;
    total_bytes +=
        metadata.sequence_keys.capacity() * sizeof(ProducerAndWriterID);
    total_bytes += metadata.sequence_losses.capacity() * sizeof(SequenceLoss);
    total_bytes += overflow_bundle_->compressed_records.capacity();
  }
  return total_bytes;
}

TraceBuffer::OverwritePolicy TraceBufferV3::overwrite_policy() const {
  return staging_buf_->overwrite_policy();
}

bool TraceBufferV3::has_data() const {
  return staging_buf_->has_data();
}

TraceBuffer::BufType TraceBufferV3::buf_type() const {
  return kV3;
}

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
