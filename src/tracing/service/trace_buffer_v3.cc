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
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
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

// The bundle target is the buffer's capacity divided by this, capped at
// kFileReadBatchBytes.
// This keeps overwrites small and leaves room for packets that compress poorly.
// A packet that crosses the target stays whole, so a bundle can exceed it.
constexpr size_t kBundleTargetDivisor = 8;

// Matches TracingServiceImpl::kWriteIntoFileChunkSize, the file-read threshold.
// Caps the bundle target and sets a floor for reusable decode blocks, so small
// buffers can reuse space across ordinary file-read batches too.
constexpr size_t kFileReadBatchBytes = 1024 * 1024;

// BundledBuffer::MaybeCompact() keeps the written range divided by this as a
// spare margin, capped at the end of the buffer.
constexpr size_t kBundledSpareCapacityDivisor = 4;

// Appends a record to the current frame and returns the record size, including
// its header.
// Consumes or copies the packet slices before returning.
size_t AppendBundleRecord(const TracePacket& packet,
                          uint32_t sequence_index,
                          uint32_t loss_reasons,
                          internal::ZstdFrameCodec* codec) {
  // Loss goes in the record header, outside the producer's bytes.
  // Appending a field could complete a truncated packet before validation.
  uint8_t record_header[3 * PERFETTO_PB_VARINT_MAX_SIZE_64];
  uint8_t* header_end = proto_utils::WriteVarInt(sequence_index, record_header);
  header_end = proto_utils::WriteVarInt(loss_reasons, header_end);
  header_end = proto_utils::WriteVarInt(packet.size(), header_end);
  const size_t header_size = static_cast<size_t>(header_end - record_header);

  codec->AddToFrame(record_header, header_size);
  for (const Slice& slice : packet.slices())
    codec->AddToFrame(slice.start, slice.size);

  return header_size + packet.size();
}

// A decoded record whose packet bytes borrow the bundle's storage.
struct BundleRecord {
  uint32_t sequence_index = 0;
  uint32_t loss_reasons = 0;
  protozero::ConstBytes packet{};
  size_t size = 0;  // Header and packet bytes.
};

// Parses one record at |offset|.
// Returns nullopt for malformed fields or packet bytes outside the bundle.
std::optional<BundleRecord> ParseBundleRecord(const uint8_t* records,
                                              size_t records_size,
                                              size_t offset,
                                              size_t num_sequences) {
  if (PERFETTO_UNLIKELY(offset >= records_size))
    return std::nullopt;

  const uint8_t* const begin = records + offset;
  const uint8_t* const end = records + records_size;
  const uint8_t* ptr = begin;

  // ParseVarInt() leaves the pointer unchanged on failure.
  uint64_t sequence_index = 0;
  const uint8_t* next = proto_utils::ParseVarInt(ptr, end, &sequence_index);
  if (PERFETTO_UNLIKELY(next == ptr || sequence_index >= num_sequences))
    return std::nullopt;
  ptr = next;

  uint64_t loss_reasons = 0;
  next = proto_utils::ParseVarInt(ptr, end, &loss_reasons);
  if (PERFETTO_UNLIKELY(next == ptr || loss_reasons > UINT32_MAX))
    return std::nullopt;
  ptr = next;

  uint64_t packet_size = 0;
  next = proto_utils::ParseVarInt(ptr, end, &packet_size);
  if (PERFETTO_UNLIKELY(next == ptr))
    return std::nullopt;

  // Bundling skips empty packets.
  if (PERFETTO_UNLIKELY(packet_size == 0))
    return std::nullopt;
  if (PERFETTO_UNLIKELY(packet_size > static_cast<size_t>(end - next)))
    return std::nullopt;

  BundleRecord record;
  record.sequence_index = static_cast<uint32_t>(sequence_index);
  record.loss_reasons = static_cast<uint32_t>(loss_reasons);
  record.packet = {next, static_cast<size_t>(packet_size)};
  record.size = static_cast<size_t>(next - begin) + record.packet.size;
  return record;
}

}  // namespace

namespace internal {

// +---------------------------------------------------------------------------+
// | BundledBuffer                                                             |
// +---------------------------------------------------------------------------+

//   +--------------+-------------------------------+--------------------+
//   | BundleHeader | sequence list                 | zstd frame         |
//   | 12 bytes     | num_sequences entries, each   | compressed records |
//   |              | a 4-byte ProducerAndWriterID  |                    |
//   +--------------+-------------------------------+--------------------+
//   |<-------------------------- stored_size -------------------------->|
//
// The sequence list stays outside the frame so an overwrite can identify
// sequences that lose packets without decoding the bundle.
// Bundles have no alignment or padding and can wrap around the buffer.
// Read headers with memcpy because they can be unaligned or split in two.
struct BundledBuffer::BundleHeader {
  uint32_t stored_size = 0;
  uint32_t uncompressed_size = 0;
  uint32_t num_sequences = 0;
};

bool BundledBuffer::Initialize(size_t capacity) {
  data_ = base::PagedMemory::Allocate(
      capacity, base::PagedMemory::kMayFail | base::PagedMemory::kDontCommit);
  if (PERFETTO_UNLIKELY(!data_.IsValid())) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", capacity);
    return false;
  }
  return true;
}

bool BundledBuffer::CloneFrom(const BundledBuffer& src) {
  if (PERFETTO_UNLIKELY(!Initialize(src.capacity())))
    return false;
  // Only the pages that receive the stored bytes are committed, as in the
  // clone of a TraceBufferV2.
  const size_t size = src.used_size();
  data_.EnsureCommitted(size);
  src.CopyBytes(/*offset_from_tail=*/0, size, data_.Get());
  wr_ = size;
  max_written_offset_ = size;
  return true;
}

uint64_t BundledBuffer::GetStoredSize(size_t num_sequences, size_t frame_size) {
  static_assert(sizeof(BundleHeader) == 12);
  static_assert(sizeof(ProducerAndWriterID) == 4);
  if (PERFETTO_UNLIKELY(num_sequences > UINT32_MAX || frame_size > UINT32_MAX))
    return UINT64_MAX;
  // Add and multiply in 64 bits before checking whether the bundle fits.
  return sizeof(BundleHeader) +
         uint64_t{num_sequences} * sizeof(ProducerAndWriterID) + frame_size;
}

void BundledBuffer::Push(uint32_t uncompressed_size,
                         const std::vector<ProducerAndWriterID>& sequence_list,
                         protozero::ConstBytes frame) {
  const uint64_t stored_size = GetStoredSize(sequence_list.size(), frame.size);
  PERFETTO_CHECK(stored_size <= UINT32_MAX);
  PERFETTO_CHECK(stored_size <= free_size());
  BundleHeader header{};
  header.stored_size = static_cast<uint32_t>(stored_size);
  header.uncompressed_size = uncompressed_size;
  header.num_sequences = static_cast<uint32_t>(sequence_list.size());
  AppendBytes(&header, sizeof(header));
  AppendBytes(sequence_list.data(),
              sequence_list.size() * sizeof(ProducerAndWriterID));
  AppendBytes(frame.data, frame.size);
}

Slice BundledBuffer::ReadFrontFrame() const {
  const BundleHeader header = ReadFrontHeader();
  const size_t frame_offset =
      sizeof(header) + header.num_sequences * sizeof(ProducerAndWriterID);
  const size_t frame_size = header.stored_size - frame_offset;
  const size_t mem_offset = OffsetOf(rd_ + frame_offset);
  if (PERFETTO_UNLIKELY(frame_size > data_.size() - mem_offset)) {
    Slice frame = Slice::Allocate(frame_size);
    CopyBytes(frame_offset, frame_size, frame.own_data());
    return frame;
  }
  return {static_cast<const uint8_t*>(data_.Get()) + mem_offset, frame_size};
}

void BundledBuffer::ReadFrontMetadata(BundleMetadata* metadata) const {
  const BundleHeader header = ReadFrontHeader();
  metadata->uncompressed_size = header.uncompressed_size;
  metadata->sequence_list.resize(header.num_sequences);
  CopyBytes(sizeof(header), header.num_sequences * sizeof(ProducerAndWriterID),
            metadata->sequence_list.data());
}

void BundledBuffer::Pop() {
  rd_ += ReadFrontHeader().stored_size;
  if (rd_ == wr_) {
    rd_ = 0;
    wr_ = 0;
  }
}

void BundledBuffer::MaybeCompact() {
  if (!empty() || max_written_offset_ == 0)
    return;

  // TODO(sashwinbalaji): TraceBufferV2::CompactChunks() has the same page
  // release logic. Share it in a helper that owns |prev_madvise_begin_|.
  const size_t spare_size =
      std::min(max_written_offset_ / kBundledSpareCapacityDivisor,
               data_.size() - max_written_offset_);
  const size_t cur_madvise_begin = max_written_offset_ + spare_size;

  // Writes since the last compaction touched pages up to |max_written_offset_|.
  prev_madvise_begin_ = std::max(prev_madvise_begin_, max_written_offset_);

  // Skip the madvise if it starts at or past |prev_madvise_begin_|, because
  // the pages from there on are already released or were never written.
  if (cur_madvise_begin < prev_madvise_begin_) {
    data_.AdviseDontNeedAfter(cur_madvise_begin);
    prev_madvise_begin_ = cur_madvise_begin;
  }
  max_written_offset_ = 0;
}

BundledBuffer::BundleHeader BundledBuffer::ReadFrontHeader() const {
  BundleHeader header{};
  PERFETTO_CHECK(used_size() >= sizeof(header));
  CopyBytes(/*offset_from_tail=*/0, sizeof(header), &header);

  // Without these sizes we cannot find the next bundle, and nothing else in
  // the buffer can be trusted either.
  PERFETTO_CHECK(header.stored_size >= sizeof(header) &&
                 header.stored_size <= used_size());
  PERFETTO_CHECK(header.num_sequences <= (header.stored_size - sizeof(header)) /
                                             sizeof(ProducerAndWriterID));
  return header;
}

void BundledBuffer::AppendBytes(const void* src, size_t size) {
  PERFETTO_DCHECK(size <= free_size());
  if (PERFETTO_UNLIKELY(size == 0))
    return;
  const size_t mem_offset = OffsetOf(wr_);
  const size_t size_to_end = std::min(size, data_.size() - mem_offset);
  data_.EnsureCommitted(mem_offset + size_to_end);

  uint8_t* mem_begin = static_cast<uint8_t*>(data_.Get());
  memcpy(mem_begin + mem_offset, src, size_to_end);
  if (PERFETTO_UNLIKELY(size > size_to_end)) {
    // The bundle wraps to the start of the memory.
    // Those pages are already committed because the buffer filled them before
    // reaching the end.
    memcpy(mem_begin, static_cast<const uint8_t*>(src) + size_to_end,
           size - size_to_end);
    max_written_offset_ = data_.size();
  } else {
    max_written_offset_ = std::max(max_written_offset_, mem_offset + size);
  }
  wr_ += size;
}

void BundledBuffer::CopyBytes(size_t offset_from_tail,
                              size_t size,
                              void* dst) const {
  PERFETTO_DCHECK(offset_from_tail <= used_size() &&
                  size <= used_size() - offset_from_tail);
  if (PERFETTO_UNLIKELY(size == 0))
    return;
  const size_t mem_offset = OffsetOf(rd_ + offset_from_tail);
  const size_t size_to_end = std::min(size, data_.size() - mem_offset);
  const uint8_t* mem_begin = static_cast<const uint8_t*>(data_.Get());
  memcpy(dst, mem_begin + mem_offset, size_to_end);
  if (PERFETTO_UNLIKELY(size > size_to_end)) {
    memcpy(static_cast<uint8_t*>(dst) + size_to_end, mem_begin,
           size - size_to_end);
  }
}

size_t BundledBuffer::OffsetOf(uint64_t position) const {
  return static_cast<size_t>(position % data_.size());
}

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+

DecodeArena::DecodeArena(size_t block_size) : block_size_(block_size) {}

uint8_t* DecodeArena::Allocate(size_t size) {
  Allocation* block = nullptr;
  for (Allocation& candidate : blocks_) {
    if (size <= block_size_ - candidate.used_bytes) {
      block = &candidate;
      break;
    }
  }

  if (!block) {
    temporaries_.push_back(
        {std::unique_ptr<uint8_t[]>(new uint8_t[size]), size});
    return temporaries_.back().data.get();
  }

  if (!block->data)
    block->data.reset(new uint8_t[block_size_]);

  uint8_t* bundle_begin = block->data.get() + block->used_bytes;
  block->used_bytes += size;
  return bundle_begin;
}

void DecodeArena::ResetExcept(const uint8_t* live_bundle) {
  if (!live_bundle) {
    for (Allocation& block : blocks_)
      block.used_bytes = 0;
    temporaries_ = std::vector<Allocation>();
    return;
  }

  // Compare integer addresses because |live_bundle| can belong to any
  // allocation in the arena.
  const uintptr_t live_bundle_addr = reinterpret_cast<uintptr_t>(live_bundle);
  for (Allocation& block : blocks_) {
    if (!block.data)
      continue;

    const uintptr_t block_begin_addr =
        reinterpret_cast<uintptr_t>(block.data.get());
    if (block_begin_addr <= live_bundle_addr &&
        live_bundle_addr < block_begin_addr + block_size_) {
      continue;
    }
    block.used_bytes = 0;
  }

  if (PERFETTO_UNLIKELY(!temporaries_.empty())) {
    // Each temporary holds exactly one bundle, starting at data.get().
    std::vector<Allocation> live_temporary;
    for (Allocation& temporary : temporaries_) {
      if (temporary.data.get() == live_bundle) {
        live_temporary.push_back(std::move(temporary));
        // Only one temporary can own |live_bundle|.
        break;
      }
    }
    // Replacing the vector also frees its spare capacity.
    temporaries_ = std::move(live_temporary);
  }
}

void DecodeArena::Compact(const uint8_t* live_bundle) {
  ResetExcept(live_bundle);
  for (Allocation& block : blocks_) {
    if (block.used_bytes == 0)
      block.data.reset();
  }
}

size_t DecodeArena::GetMemoryUsageBytes() const {
  size_t total_bytes = blocks_.size() * block_size_;
  for (const Allocation& temporary : temporaries_)
    total_bytes += temporary.used_bytes;
  return total_bytes;
}

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::Create(size_t size_in_bytes,
                                                     OverwritePolicy policy) {
  // Use the same page alignment as TraceBufferV2 for both buffers.
  const size_t capacity =
      base::AlignUp(std::max(size_in_bytes, size_t(1)), 4096);
  Params params{};
  params.staging_capacity_bytes = capacity;
  params.bundle_target_bytes =
      std::min(capacity / kBundleTargetDivisor, kFileReadBatchBytes);
  return CreateInternal(capacity, policy, params);
}

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::CreateInternal(
    size_t size_in_bytes,
    OverwritePolicy policy,
    const Params& params) {
  const size_t capacity = std::max(size_in_bytes, size_t(1));
  // Stored bundle sizes are 32-bit.
  // A bundle is stored only if it fits the bundled buffer, so this limit
  // keeps its stored size representable.
  PERFETTO_CHECK(capacity <= UINT32_MAX);
  std::unique_ptr<TraceBufferV3> buf(new TraceBufferV3(params));
  buf->staging_buf_ =
      TraceBufferV2::Create(params.staging_capacity_bytes, policy);
  if (PERFETTO_UNLIKELY(!buf->staging_buf_ ||
                        !buf->bundled_buf_.Initialize(capacity))) {
    return nullptr;
  }
  return buf;
}

TraceBufferV3::TraceBufferV3(const Params& params)
    : params_(params),
      codec_(new internal::ZstdFrameCodec(params.bundle_target_bytes)),
      // A crossing record up to the target, including its header, still fits.
      decode_arena_(
          std::max(kFileReadBatchBytes, 2 * params.bundle_target_bytes)) {}

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
  if (!chunk)
    return;
  CopyPreparedChunk(*chunk);
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
  if (!chunk)
    return false;
  return CopyPreparedChunk(*chunk);
}

void TraceBufferV3::RecordChunkV2DataLoss(ProducerID producer_id,
                                          WriterID writer_id) {
  staging_buf_->RecordChunkV2DataLoss(producer_id, writer_id);
}

void TraceBufferV3::RecordAbiViolation() {
  staging_buf_->RecordAbiViolation();
}

bool TraceBufferV3::CopyPreparedChunk(
    const TraceBufferV2::PreparedChunk& chunk) {
  using WriteMode = TraceBufferV2::WriteMode;
  using WriteResult = TraceBufferV2::WriteResult;
  // Count only new TBChunks.
  // An in-place recommit returns kAccepted but leaves staging's bytes_written
  // unchanged.
  const uint64_t bytes_before = staging_buf_->stats().bytes_written();

  auto result = staging_buf_->TryCopyChunk(chunk, WriteMode::kAvoidOverwrite);
  if (PERFETTO_UNLIKELY(result == WriteResult::kNeedsSpace)) {
    BundleStagingForWrite(WriteBundlingReason::kStagingPressure);
    // Reading and compaction can remove or move a scraped chunk.
    // The input holds no staging pointers, so retry its lookup.
    // The retry applies the fill policy if staging still has no space.
    // It also rejects the chunk if the pass stopped intake.
    result = staging_buf_->TryCopyChunk(chunk, WriteMode::kAllowOverwrite);
  }

  const uint64_t bytes_written =
      staging_buf_->stats().bytes_written() - bytes_before;
  if (bytes_since_checkpoint_pass_ < params_.bundle_target_bytes)
    bytes_since_checkpoint_pass_ += static_cast<size_t>(bytes_written);

  // A kDiscard retry that cannot fit stops staging intake.
  // Stop write-side bundling too.
  if (PERFETTO_UNLIKELY(staging_buf_->discard_writes()))
    StopAcceptingWrites();
  return result == WriteResult::kAccepted;
}

void TraceBufferV3::StopAcceptingWrites() {
  PERFETTO_DCHECK(overwrite_policy() == kDiscard);
  discard_writes_ = true;
  staging_buf_->StopAcceptingWrites();
}

void TraceBufferV3::OnWriteCheckpoint() {
  PERFETTO_DCHECK(discard_writes_ == staging_buf_->discard_writes());

  // kDiscard stops write-side bundling once intake stops.
  // Reads can still bundle all accepted packets.
  if (PERFETTO_UNLIKELY(discard_writes_))
    return;

  if (bytes_since_checkpoint_pass_ < params_.bundle_target_bytes)
    return;

  bytes_since_checkpoint_pass_ = 0;
  BundleStagingForWrite(WriteBundlingReason::kCheckpoint);
}

void TraceBufferV3::BundleStagingForWrite(WriteBundlingReason reason) {
  if (PERFETTO_UNLIKELY(discard_writes_))
    return;

  const base::TimeNanos start = base::GetWallTimeNs();
  const uint64_t compactions_skipped =
      staging_buf_->stats().compactions_skipped();

  // See WriteBundlingReason::kStagingPressure for why a pressure pass waits at
  // SMB v1 gaps.
  const auto read_mode = reason == WriteBundlingReason::kStagingPressure
                             ? ChunkSeqReader::kWaitForMissingChunks
                             : ChunkSeqReader::kReadMode;
  staging_buf_->BeginRead(read_mode);

  // Stop when the compressed allowance is exhausted, so at most one bundle
  // waits as |overflow_bundle_|.
  while (!discard_writes_) {
    if (BundleNextFromStaging() == BundleResult::kStagingReadComplete) {
      staging_buf_->CompactChunks();
      break;
    }
  }
  const uint64_t duration_us = RecordBundlingPass(start);
  if (reason == WriteBundlingReason::kStagingPressure) {
    ++bundle_counters_.staging_pressure_passes;
    bundle_counters_.staging_pressure_time_us += duration_us;
    bundle_counters_.staging_pressure_compactions_skipped +=
        staging_buf_->stats().compactions_skipped() - compactions_skipped;
  }
}

bool TraceBufferV3::BundleStagingForRead() {
  PERFETTO_DCHECK(bundled_buf_.empty() && !overflow_bundle_);
  if (staging_read_complete_)
    return false;

  const base::TimeNanos start = base::GetWallTimeNs();

  // Decode each stored bundle before creating another that could overwrite it.
  while (bundled_buf_.empty()) {
    staging_read_complete_ =
        BundleNextFromStaging() == BundleResult::kStagingReadComplete;
    if (staging_read_complete_) {
      bytes_since_checkpoint_pass_ = 0;
      staging_buf_->CompactChunks();
      break;
    }
  }
  RecordBundlingPass(start);
  return !bundled_buf_.empty();
}

uint64_t TraceBufferV3::RecordBundlingPass(base::TimeNanos start) {
  const uint64_t duration_us =
      static_cast<uint64_t>((base::GetWallTimeNs() - start).count() / 1000);
  bundle_counters_.bundling_max_us =
      std::max(bundle_counters_.bundling_max_us, duration_us);
  bundle_counters_.bundling_time_us.Add(static_cast<HistValue>(duration_us));
  return duration_us;
}

TraceBufferV3::BundleResult TraceBufferV3::BundleNextFromStaging() {
  PERFETTO_DCHECK(open_bundle_.sequence_list.empty());
  PERFETTO_DCHECK(open_bundle_.sequences.size() == 0);
  PERFETTO_DCHECK(open_bundle_.uncompressed_size == 0);
  codec_->BeginFrame();

  // Cache the entry only until a different sequence needs a lookup.
  // Inserting into |open_bundle_.sequences| can move its entries.
  ProducerAndWriterID last_seq_key = 0;
  OpenBundle::Sequence* sequence = nullptr;
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

    const ProducerAndWriterID seq_key = MkProducerAndWriterID(
        properties.producer_id_trusted, properties.writer_id);

    // SMB v1 can reassemble an empty packet from empty fragments.
    // Keep its loss for the next packet that leaves staging.
    if (PERFETTO_UNLIKELY(staged_packet_.size() == 0)) {
      if (loss_reasons)
        staging_buf_->RecordSequenceDataLoss(seq_key, loss_reasons);
      continue;
    }

    if (!sequence || seq_key != last_seq_key) {
      sequence = GetOrAddOpenBundleSequence(seq_key);
      last_seq_key = seq_key;
    }
    sequence->loss_reasons |= loss_reasons;
    open_bundle_.uncompressed_size += AppendBundleRecord(
        staged_packet_, sequence->index, loss_reasons, codec_.get());

    // Keep the packet that crosses the target whole.
    if (open_bundle_.uncompressed_size >= params_.bundle_target_bytes)
      break;
  }

  // The codec has consumed or copied the slices.
  // Drop them before staging can move or erase their chunks.
  staged_packet_.Clear();
  FinishOpenBundle();
  return result;
}

TraceBufferV3::OpenBundle::Sequence* TraceBufferV3::GetOrAddOpenBundleSequence(
    ProducerAndWriterID seq_key) {
  auto [sequence, inserted] = open_bundle_.sequences.Insert(seq_key, {});
  if (!inserted)
    return sequence;

  sequence->index = static_cast<uint32_t>(open_bundle_.sequence_list.size());
  open_bundle_.sequence_list.push_back(seq_key);

  auto [bundled_sequence, new_sequence] =
      bundled_sequences_.Insert(seq_key, {});
  if (new_sequence)
    staging_buf_->PreventSequencePruning(seq_key);
  ++bundled_sequence->bundle_ref_count;
  return sequence;
}

void TraceBufferV3::ReleaseSequenceRef(ProducerAndWriterID seq_key,
                                       uint32_t loss_reasons) {
  auto* sequence = bundled_sequences_.Find(seq_key);
  if (PERFETTO_UNLIKELY(!sequence))
    return;  // Only for a corrupted bundle.

  sequence->unreported_loss_reasons |= loss_reasons;
  PERFETTO_DCHECK(sequence->bundle_ref_count > 0);
  --sequence->bundle_ref_count;

  if (sequence->bundle_ref_count > 0)
    return;

  // The next packet on this sequence now comes from staging.
  // Return its pending loss and let staging decide when to prune its state.
  if (sequence->unreported_loss_reasons) {
    staging_buf_->RecordSequenceDataLoss(seq_key,
                                         sequence->unreported_loss_reasons);
  }
  staging_buf_->AllowSequencePruning(seq_key);
  bundled_sequences_.Erase(seq_key);
}

void TraceBufferV3::FinishOpenBundle() {
  // Keep the list until its references have been transferred or released.
  auto reset_open_bundle = base::OnScopeExit([this] {
    open_bundle_.sequence_list.clear();
    open_bundle_.sequences.Clear();
    open_bundle_.uncompressed_size = 0;
  });

  if (open_bundle_.sequence_list.empty())
    return;

  const protozero::ConstBytes frame = codec_->EndFrame();
  const uint64_t stored_size = internal::BundledBuffer::GetStoredSize(
      open_bundle_.sequence_list.size(), frame.size);

  // Evicting older bundles cannot help a bundle larger than the buffer.
  // The decoded size must also fit the bundle header's 32-bit field.
  if (PERFETTO_UNLIKELY(stored_size > bundled_buf_.capacity() ||
                        open_bundle_.uncompressed_size > UINT32_MAX)) {
    RecoverOpenBundleProducerLoss(frame);
    for (ProducerAndWriterID seq_key : open_bundle_.sequence_list) {
      const auto* sequence = open_bundle_.sequences.Find(seq_key);
      PERFETTO_DCHECK(sequence);
      // The next packet from staging must report this loss.
      // Packets already in bundles are older than the dropped bundle.
      staging_buf_->RecordSequenceDataLoss(
          seq_key, DataLossReason::DATA_LOSS_PRESENT | sequence->loss_reasons);
      ReleaseSequenceRef(seq_key, /*loss_reasons=*/0);
    }
    ++bundle_counters_.bundles_too_large;
    return;
  }

  const uint32_t uncompressed_size =
      static_cast<uint32_t>(open_bundle_.uncompressed_size);
  ++bundle_counters_.bundles_written;
  bundle_counters_.bundle_bytes_uncompressed += uncompressed_size;
  bundle_counters_.bundle_bytes_stored += stored_size;
  bundle_counters_.max_bundle_uncompressed_size = std::max(
      bundle_counters_.max_bundle_uncompressed_size, uncompressed_size);

  // A consumer read frees space in |bundled_buf_| but does not restore the
  // compressed allowance.
  // When the allowance is exhausted, intake stops, but this bundle and every
  // accepted packet stay readable.
  if (overwrite_policy() == kDiscard && !discard_writes_ &&
      bundle_counters_.bundle_bytes_stored >= bundled_buf_.capacity()) {
    StopAcceptingWrites();
  }

  if (overwrite_policy() == kOverwrite) {
    while (stored_size > bundled_buf_.free_size())
      RemoveFrontBundle(RemoveReason::kOverwritten);
  } else if (stored_size > bundled_buf_.free_size()) {
    // These packets left staging already, so retain them for readout.
    // The cumulative byte count includes the stored bundles and this one.
    // It exceeds capacity here, so intake has already stopped.
    PERFETTO_DCHECK(discard_writes_ && !overflow_bundle_);
    overflow_bundle_.emplace();
    overflow_bundle_->metadata.uncompressed_size = uncompressed_size;
    overflow_bundle_->metadata.sequence_list = open_bundle_.sequence_list;
    overflow_bundle_->frame.assign(frame.data, frame.data + frame.size);
    return;
  }

  bundled_buf_.Push(uncompressed_size, open_bundle_.sequence_list, frame);
}

void TraceBufferV3::RecoverOpenBundleProducerLoss(protozero::ConstBytes frame) {
  if (open_bundle_.uncompressed_size > UINT32_MAX)
    return;

  const size_t records_size =
      static_cast<size_t>(open_bundle_.uncompressed_size);
  std::unique_ptr<uint8_t[]> records(new uint8_t[records_size]);
  if (!codec_->Decompress(frame, records.get(), records_size))
    return;

  for (size_t offset = 0; offset < records_size;) {
    const auto record = ParseBundleRecord(records.get(), records_size, offset,
                                          open_bundle_.sequence_list.size());
    if (PERFETTO_UNLIKELY(!record))
      return;
    offset += record->size;

    // Read the producer's loss field without changing or validating the packet.
    protozero::ProtoDecoder decoder(record->packet.data, record->packet.size);
    uint32_t producer_loss = 0;
    for (auto field = decoder.ReadField(); field.valid();
         field = decoder.ReadField()) {
      if (field.id() !=
              protos::pbzero::TracePacket::kPreviousPacketDroppedFieldNumber ||
          field.type() != proto_utils::ProtoWireType::kVarInt) {
        continue;
      }
      producer_loss = field.as_uint32();
    }

    const ProducerAndWriterID seq_key =
        open_bundle_.sequence_list[record->sequence_index];
    auto* sequence = open_bundle_.sequences.Find(seq_key);
    PERFETTO_DCHECK(sequence);
    sequence->loss_reasons |= producer_loss;
  }
}

void TraceBufferV3::RemoveFrontBundle(RemoveReason reason) {
  bundled_buf_.ReadFrontMetadata(&front_bundle_metadata_);

  uint32_t loss_reasons = 0;
  switch (reason) {
    case RemoveReason::kOverwritten:
      // TODO(sashwinbalaji): Also count the uncompressed bytes of overwritten
      // bundles and add them to long_trace_mode_bytes_overwritten in TP.
      ++bundle_counters_.bundles_overwritten;
      ProcessOverwrittenBundle(front_bundle_metadata_);
      loss_reasons = DataLossReason::DATA_LOSS_PRESENT |
                     DataLossReason::DATA_LOSS_OVERWRITE;
      break;
    case RemoveReason::kCorrupted:
      ++bundle_counters_.bundles_corrupted;
      loss_reasons = DataLossReason::DATA_LOSS_PRESENT |
                     DataLossReason::DATA_LOSS_CHUNK_CORRUPTED;
      break;
  }

  for (ProducerAndWriterID seq_key : front_bundle_metadata_.sequence_list)
    ReleaseSequenceRef(seq_key, loss_reasons);

  bundled_buf_.Pop();
}

void TraceBufferV3::ProcessOverwrittenBundle(const BundleMetadata& metadata) {
  if (protovms_.empty())
    return;

  const auto& sequence_list = metadata.sequence_list;
  if (std::none_of(sequence_list.begin(), sequence_list.end(),
                   [this](ProducerAndWriterID seq_key) {
                     ProducerID producer_id = 0;
                     WriterID writer_id = 0;
                     GetProducerAndWriterID(seq_key, &producer_id, &writer_id);
                     return protovms_.HasProducer(producer_id);
                   })) {
    return;
  }

  // Validate the size before allocating decoded storage.
  // The caller reports overwrite loss on every sequence either way.
  if (PERFETTO_UNLIKELY(metadata.uncompressed_size >
                        bundle_counters_.max_bundle_uncompressed_size)) {
    return;
  }

  // TODO(sashwinbalaji): Reuse one decode buffer here and in the too-large
  // path instead of allocating one per evicted bundle.
  std::unique_ptr<uint8_t[]> records(new uint8_t[metadata.uncompressed_size]);
  const auto frame = bundled_buf_.ReadFrontFrame();
  const bool decompressed = codec_->Decompress(
      /*frame=*/{static_cast<const uint8_t*>(frame.start), frame.size},
      /*dst=*/records.get(), /*dst_size=*/metadata.uncompressed_size);
  if (PERFETTO_UNLIKELY(!decompressed))
    return;

  // Look up the producer once per run of packets from the same sequence.
  uint32_t last_sequence_index = UINT32_MAX;  // Not a valid index.
  ProducerID producer_id = 0;
  bool producer_has_protovm = false;
  TracePacket packet;
  for (size_t offset = 0; offset < metadata.uncompressed_size;) {
    const auto record =
        ParseBundleRecord(records.get(), metadata.uncompressed_size, offset,
                          sequence_list.size());
    // The caller reports loss on every sequence if records are malformed.
    if (PERFETTO_UNLIKELY(!record))
      return;
    offset += record->size;

    if (record->sequence_index != last_sequence_index) {
      last_sequence_index = record->sequence_index;
      WriterID writer_id = 0;
      GetProducerAndWriterID(sequence_list[last_sequence_index], &producer_id,
                             &writer_id);
      producer_has_protovm = protovms_.HasProducer(producer_id);
    }
    if (!producer_has_protovm)
      continue;

    packet.Clear();
    packet.AddSlice(record->packet.data, record->packet.size);
    protovms_.MaybeProcessOverwrittenPacket(packet, producer_id);
  }
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
  *sequence_properties = {0, ClientIdentity(), 0};
  *previous_packet_on_sequence_dropped = 0;

  for (;;) {
    if (!decoded_bundle_.records && !ReadNextBundle())
      return false;

    const auto record = ParseBundleRecord(
        decoded_bundle_.records, decoded_bundle_.uncompressed_size,
        decoded_bundle_.read_offset, decoded_bundle_.sequences.size());
    if (PERFETTO_UNLIKELY(!record)) {
      ++bundle_counters_.bundles_corrupted;
      ReleaseDecodedBundle(DataLossReason::DATA_LOSS_PRESENT |
                           DataLossReason::DATA_LOSS_CHUNK_CORRUPTED);
      continue;
    }

    decoded_bundle_.read_offset += record->size;
    auto& sequence = decoded_bundle_.sequences[record->sequence_index];
    for (size_t offset = 0; offset < record->packet.size;
         offset += kReadSliceSize) {
      packet->AddSlice(record->packet.data + offset,
                       std::min(kReadSliceSize, record->packet.size - offset));
    }

    *sequence_properties = sequence.properties;
    *previous_packet_on_sequence_dropped =
        sequence.loss_reasons | record->loss_reasons;
    sequence.loss_reasons = 0;
    bundle_counters_.bytes_read += record->size;

    // Returned slices stay in the arena until the next BeginRead() or
    // MaybeCompact().
    if (decoded_bundle_.read_offset == decoded_bundle_.uncompressed_size)
      ReleaseDecodedBundle(/*loss_reasons=*/0);
    return true;
  }
}

bool TraceBufferV3::ReadNextBundle() {
  PERFETTO_DCHECK(!decoded_bundle_.records);

  for (;;) {
    if (bundled_buf_.empty()) {
      if (overflow_bundle_) {
        // Readout decodes every bundle from |bundled_buf_|.
        bundled_buf_.Push(
            overflow_bundle_->metadata.uncompressed_size,
            overflow_bundle_->metadata.sequence_list,
            {overflow_bundle_->frame.data(), overflow_bundle_->frame.size()});
        overflow_bundle_.reset();
      } else if (!BundleStagingForRead()) {
        return false;
      }
    }

    if (PERFETTO_LIKELY(DecodeFrontBundle()))
      return true;

    // DecodeFrontBundle() removed the corrupt bundle.
    // Try the next available bundle.
  }
}

bool TraceBufferV3::DecodeFrontBundle() {
  PERFETTO_DCHECK(!decoded_bundle_.records);
  PERFETTO_DCHECK(decoded_bundle_.read_offset == 0);
  PERFETTO_DCHECK(decoded_bundle_.uncompressed_size == 0);
  PERFETTO_DCHECK(decoded_bundle_.sequences.empty());

  bundled_buf_.ReadFrontMetadata(&front_bundle_metadata_);
  const BundleMetadata& metadata = front_bundle_metadata_;
  const uint32_t uncompressed_size = metadata.uncompressed_size;

  // Validate the size before allocating decoded storage.
  if (PERFETTO_UNLIKELY(uncompressed_size == 0 ||
                        uncompressed_size >
                            bundle_counters_.max_bundle_uncompressed_size)) {
    RemoveFrontBundle(RemoveReason::kCorrupted);
    return false;
  }

  // GetSequenceClientIdentity() CHECKs that the sequence exists in staging.
  // Reject a corrupt list before looking up those identities.
  for (ProducerAndWriterID seq_key : metadata.sequence_list) {
    if (PERFETTO_UNLIKELY(!bundled_sequences_.Find(seq_key))) {
      RemoveFrontBundle(RemoveReason::kCorrupted);
      return false;
    }
  }

  uint8_t* records = decode_arena_.Allocate(uncompressed_size);
  const auto frame = bundled_buf_.ReadFrontFrame();
  const bool decompressed = codec_->Decompress(
      /*frame=*/{static_cast<const uint8_t*>(frame.start), frame.size},
      /*dst=*/records, /*dst_size=*/uncompressed_size);
  if (PERFETTO_UNLIKELY(!decompressed)) {
    RemoveFrontBundle(RemoveReason::kCorrupted);
    return false;
  }

  decoded_bundle_.records = records;
  decoded_bundle_.uncompressed_size = uncompressed_size;
  decoded_bundle_.sequences.reserve(metadata.sequence_list.size());
  for (ProducerAndWriterID seq_key : metadata.sequence_list) {
    DecodedBundle::Sequence sequence{};
    sequence.seq_key = seq_key;
    GetProducerAndWriterID(seq_key, &sequence.properties.producer_id_trusted,
                           &sequence.properties.writer_id);
    sequence.properties.client_identity_trusted =
        staging_buf_->GetSequenceClientIdentity(seq_key);
    // Take only the loss that preceded this bundle.
    // Later overwrites add loss to the map, leaving this snapshot unchanged.
    auto* bundled_sequence = bundled_sequences_.Find(seq_key);
    PERFETTO_DCHECK(bundled_sequence);
    sequence.loss_reasons = bundled_sequence->unreported_loss_reasons;
    bundled_sequence->unreported_loss_reasons = 0;
    decoded_bundle_.sequences.push_back(sequence);
  }

  // The arena holds the only copy of the unread packets after this pop.
  // Later writes can overwrite newer stored bundles while this read continues.
  // Their loss stays in the sequence map, after the snapshot above.
  bundled_buf_.Pop();
  return true;
}

void TraceBufferV3::ReleaseDecodedBundle(uint32_t loss_reasons) {
  for (const auto& sequence : decoded_bundle_.sequences)
    ReleaseSequenceRef(sequence.seq_key, sequence.loss_reasons | loss_reasons);
  decoded_bundle_.read_offset = 0;
  decoded_bundle_.records = nullptr;
  decoded_bundle_.uncompressed_size = 0;
  decoded_bundle_.sequences.clear();
}

void TraceBufferV3::MaybeCompact() {
  decode_arena_.Compact(decoded_bundle_.records);
  staging_buf_->CompactChunks();
  bundled_buf_.MaybeCompact();
}

std::unique_ptr<TraceBuffer> TraceBufferV3::CloneReadOnly() const {
  std::unique_ptr<TraceBufferV3> clone(new TraceBufferV3(params_));
  clone->staging_buf_.reset(
      static_cast<TraceBufferV2*>(staging_buf_->CloneReadOnly().release()));
  if (PERFETTO_UNLIKELY(!clone->staging_buf_))
    return nullptr;
  if (PERFETTO_UNLIKELY(!clone->bundled_buf_.CloneFrom(bundled_buf_)))
    return nullptr;

  clone->discard_writes_ = discard_writes_;
  clone->overflow_bundle_ = overflow_bundle_;
  for (auto it = bundled_sequences_.GetIterator(); it; ++it)
    clone->bundled_sequences_.Insert(it.key(), it.value());
  clone->protovms_.CloneReadOnlyFrom(protovms_);
  clone->bundle_counters_ = bundle_counters_;
  clone->bundle_counters_.bytes_read = 0;

  if (!decoded_bundle_.records)
    return clone;

  // The decoded bundle has no copy left in compressed storage.
  clone->decoded_bundle_ = decoded_bundle_;
  uint8_t* records =
      clone->decode_arena_.Allocate(decoded_bundle_.uncompressed_size);
  memcpy(records, decoded_bundle_.records, decoded_bundle_.uncompressed_size);
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
  stats_.set_buffer_size(bundled_buf_.capacity());

  // In staging, a read moves packets into bundles.
  // Only the packets that ReadNextTracePacket() returned count as read by the
  // consumer.
  stats_.set_staging_bytes_read(stats_.bytes_read());
  stats_.set_staging_chunks_read(stats_.chunks_read());
  stats_.set_staging_readaheads_succeeded(stats_.readaheads_succeeded());
  stats_.set_staging_readaheads_failed(stats_.readaheads_failed());
  stats_.set_bytes_read(bundle_counters_.bytes_read);
  stats_.set_chunks_read(0);
  stats_.set_readaheads_succeeded(0);
  stats_.set_readaheads_failed(0);

  stats_.set_bundles_written(bundle_counters_.bundles_written);
  stats_.set_bundle_bytes_uncompressed(
      bundle_counters_.bundle_bytes_uncompressed);
  stats_.set_bundle_bytes_stored(bundle_counters_.bundle_bytes_stored);
  stats_.set_bundles_overwritten(bundle_counters_.bundles_overwritten);
  stats_.set_bundles_too_large(bundle_counters_.bundles_too_large);
  stats_.set_bundles_corrupted(bundle_counters_.bundles_corrupted);
  stats_.set_bundling_max_us(bundle_counters_.bundling_max_us);
  stats_.set_staging_pressure_passes(bundle_counters_.staging_pressure_passes);
  stats_.set_staging_pressure_time_us(
      bundle_counters_.staging_pressure_time_us);
  stats_.set_staging_pressure_compactions_skipped(
      bundle_counters_.staging_pressure_compactions_skipped);
  const auto& hist = bundle_counters_.bundling_time_us;
  for (size_t i = 0; i < hist.num_buckets(); ++i) {
    if (i + 1 < hist.num_buckets())
      stats_.add_bundling_time_us_histogram_def(hist.GetBucketThres(i));
    stats_.add_bundling_time_us_histogram_counts(hist.GetBucketCount(i));
    stats_.add_bundling_time_us_histogram_sum(hist.GetBucketSum(i));
  }
  return stats_;
}

const TraceBuffer::WriterStats& TraceBufferV3::writer_stats() const {
  return staging_buf_->writer_stats();
}

size_t TraceBufferV3::size() const {
  return bundled_buf_.capacity();
}

size_t TraceBufferV3::used_size() const {
  return bundled_buf_.used_size();
}

size_t TraceBufferV3::GetMemoryUsageBytes() const {
  size_t total_bytes = bundled_buf_.capacity();
  total_bytes += staging_buf_->GetMemoryUsageBytes();
  total_bytes += protovms_.GetMemoryUsageBytes();
  total_bytes += codec_->GetMemoryUsageBytes();
  total_bytes += decode_arena_.GetMemoryUsageBytes();
  if (overflow_bundle_) {
    total_bytes += overflow_bundle_->metadata.sequence_list.capacity() *
                   sizeof(ProducerAndWriterID);
    total_bytes += overflow_bundle_->frame.capacity();
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
