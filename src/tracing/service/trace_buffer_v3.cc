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
#include <cinttypes>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/service/packet_stream_validator.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/service/zstd_frame_codec.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace proto_utils = ::protozero::proto_utils;

namespace perfetto {

using DataLossReason = protos::pbzero::TracePacket_DataLossReason;

namespace {

// The names below must not collide with trace_buffer_v{1,2}.cc in the
// amalgamated builds.

// A bundle aims for 1/8 of the bundled buffer's capacity before compression,
// subject to the limits below. This keeps overwrites small and leaves room for
// packets that compress poorly. A packet that crosses the target stays whole,
// so a bundle can exceed it.
constexpr size_t kBundleTargetDivisor = 8;
constexpr size_t kMinBundleTargetBytes = 512;
constexpr size_t kMaxBundleTargetBytes = 512 * 1024;

// TraceBufferV2 rejects a chunk larger than its buffer. The largest SMB chunk
// takes 64 KiB plus a header, so staging needs more than that.
constexpr size_t kMinStagingBytes = 128 * 1024;
constexpr size_t kMaxStagingBytes = 8 * 1024 * 1024;

// See BundledBuffer::ReleaseUnusedPages().
constexpr size_t kBundledSpareCapacityDivisor = 4;

constexpr size_t kMaxVarIntSize = 10;

// What the sequences of a removed bundle report for their unread packets.
constexpr uint32_t kOverwriteLossReasons =
    DataLossReason::DATA_LOSS_PRESENT | DataLossReason::DATA_LOSS_OVERWRITE;
constexpr uint32_t kCorruptedLossReasons =
    DataLossReason::DATA_LOSS_PRESENT |
    DataLossReason::DATA_LOSS_CHUNK_CORRUPTED;

// One record of a decoded bundle, a header of two varints followed by the
// packet. See BundleHeader in the .h file.
struct BundleRecord {
  uint32_t sequence_index = 0;
  const uint8_t* packet = nullptr;
  size_t packet_size = 0;
  size_t record_size = 0;  // The record header and the packet.
};

// Writes the header of a record into |dst|, which must have room for
// 2 * kMaxVarIntSize bytes. Returns the size of the header.
size_t WriteBundleRecordHeader(uint32_t sequence_index,
                               size_t packet_size,
                               uint8_t* dst) {
  uint8_t* end = proto_utils::WriteVarInt(sequence_index, dst);
  end = proto_utils::WriteVarInt(packet_size, end);
  return static_cast<size_t>(end - dst);
}

// Parses the record at |offset| of |records|. Returns nullopt if the record
// is malformed. The records come out of a zstd frame that this process wrote,
// so that means a bug or corrupted memory. The reader must still not follow a
// bad size or index.
std::optional<BundleRecord> ParseBundleRecord(const uint8_t* records,
                                              size_t records_size,
                                              size_t offset,
                                              size_t num_sequences) {
  PERFETTO_DCHECK(offset < records_size);
  const uint8_t* const begin = records + offset;
  const uint8_t* const end = records + records_size;

  // ParseVarInt() returns its first argument if it finds no valid varint.
  uint64_t sequence_index = 0;
  const uint8_t* size_begin =
      proto_utils::ParseVarInt(begin, end, &sequence_index);
  uint64_t packet_size = 0;
  const uint8_t* packet =
      proto_utils::ParseVarInt(size_begin, end, &packet_size);
  if (PERFETTO_UNLIKELY(size_begin == begin || packet == size_begin))
    return std::nullopt;

  // BuildBundle() never bundles an empty packet, because
  // ReadNextTracePacket() must return at least one slice. So a record with
  // no packet bytes is corrupt.
  if (PERFETTO_UNLIKELY(sequence_index >= num_sequences || packet_size == 0 ||
                        packet_size > static_cast<size_t>(end - packet))) {
    return std::nullopt;
  }
  BundleRecord record{};
  record.sequence_index = static_cast<uint32_t>(sequence_index);
  record.packet = packet;
  record.packet_size = static_cast<size_t>(packet_size);
  record.record_size = static_cast<size_t>(packet - begin) + record.packet_size;
  return record;
}

}  // namespace

namespace internal {

// +---------------------------------------------------------------------------+
// | BundledBuffer                                                             |
// +---------------------------------------------------------------------------+

bool BundledBuffer::Initialize(size_t capacity) {
  mem_ = base::PagedMemory::Allocate(
      capacity, base::PagedMemory::kMayFail | base::PagedMemory::kDontCommit);
  if (PERFETTO_UNLIKELY(!mem_.IsValid())) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", capacity);
    return false;
  }
  return true;
}

bool BundledBuffer::InitializeAsCloneOf(const BundledBuffer& src) {
  if (PERFETTO_UNLIKELY(!Initialize(src.capacity())))
    return false;
  // Only the pages that receive the stored bytes are committed, as in the
  // clone of a TraceBufferV2.
  const size_t size = src.used_size();
  mem_.EnsureCommitted(size);
  src.CopyOut(0, size, mem_.Get());
  wr_ = size;
  max_written_offset_ = size;
  return true;
}

void BundledBuffer::Append(const void* src, size_t size) {
  PERFETTO_DCHECK(size <= free_size());
  if (size == 0)
    return;
  const size_t mem_offset = OffsetOf(wr_);
  const size_t size_to_end = std::min(size, mem_.size() - mem_offset);
  mem_.EnsureCommitted(mem_offset + size_to_end);

  uint8_t* mem_begin = static_cast<uint8_t*>(mem_.Get());
  memcpy(mem_begin + mem_offset, src, size_to_end);
  if (size > size_to_end) {
    // The bundle wraps to the start of the memory. Those pages are already
    // committed because the buffer filled them before reaching the end.
    memcpy(mem_begin, static_cast<const uint8_t*>(src) + size_to_end,
           size - size_to_end);
    max_written_offset_ = mem_.size();
  } else {
    max_written_offset_ = std::max(max_written_offset_, mem_offset + size);
  }
  wr_ += size;
}

void BundledBuffer::CopyOut(size_t offset_from_rd,
                            size_t size,
                            void* dst) const {
  PERFETTO_DCHECK(offset_from_rd <= used_size() &&
                  size <= used_size() - offset_from_rd);
  if (size == 0)
    return;
  const size_t mem_offset = OffsetOf(rd_ + offset_from_rd);
  const size_t size_to_end = std::min(size, mem_.size() - mem_offset);
  const uint8_t* mem_begin = static_cast<const uint8_t*>(mem_.Get());
  memcpy(dst, mem_begin + mem_offset, size_to_end);
  if (size > size_to_end) {
    memcpy(static_cast<uint8_t*>(dst) + size_to_end, mem_begin,
           size - size_to_end);
  }
}

const uint8_t* BundledBuffer::PeekContiguous(size_t offset_from_rd,
                                             size_t size) const {
  PERFETTO_DCHECK(offset_from_rd <= used_size() &&
                  size <= used_size() - offset_from_rd);
  const size_t mem_offset = OffsetOf(rd_ + offset_from_rd);
  if (size > mem_.size() - mem_offset)
    return nullptr;
  return static_cast<const uint8_t*>(mem_.Get()) + mem_offset;
}

void BundledBuffer::Consume(size_t size) {
  PERFETTO_DCHECK(size <= used_size());
  rd_ += size;
  if (rd_ == wr_) {
    rd_ = 0;
    wr_ = 0;
  }
}

void BundledBuffer::ReleaseUnusedPages() {
  if (!empty() || max_written_offset_ == 0)
    return;
  const size_t spare_size =
      std::min(max_written_offset_ / kBundledSpareCapacityDivisor,
               mem_.size() - max_written_offset_);
  mem_.AdviseDontNeedAfter(max_written_offset_ + spare_size);
  max_written_offset_ = 0;
}

// +---------------------------------------------------------------------------+
// | DecodeArena                                                               |
// +---------------------------------------------------------------------------+

DecodeArena::DecodeArena(size_t block_size) : block_size_(block_size) {}

uint8_t* DecodeArena::Allocate(size_t size) {
  for (Allocation& block : blocks_) {
    if (size > block_size_ - block.used)
      continue;
    if (!block.data)
      block.data.reset(new uint8_t[block_size_]);
    uint8_t* ptr = block.data.get() + block.used;
    block.used += size;
    return ptr;
  }
  temporaries_.push_back({std::unique_ptr<uint8_t[]>(new uint8_t[size]), size});
  return temporaries_.back().data.get();
}

void DecodeArena::ResetExcept(const uint8_t* live_bundle) {
  for (Allocation& block : blocks_) {
    // |live_bundle| can belong to another allocation. Compare addresses as
    // integers to check whether it falls within this block.
    const uintptr_t begin = reinterpret_cast<uintptr_t>(block.data.get());
    const uintptr_t live_addr = reinterpret_cast<uintptr_t>(live_bundle);
    const bool holds_live_bundle = live_bundle && block.data &&
                                   live_addr >= begin &&
                                   live_addr - begin < block_size_;
    if (!holds_live_bundle)
      block.used = 0;
  }
  temporaries_.erase(std::remove_if(temporaries_.begin(), temporaries_.end(),
                                    [live_bundle](const Allocation& temporary) {
                                      return temporary.data.get() !=
                                             live_bundle;
                                    }),
                     temporaries_.end());
}

void DecodeArena::ReleaseEmptyBlocks() {
  for (Allocation& block : blocks_) {
    if (block.used == 0)
      block.data.reset();
  }
}

size_t DecodeArena::GetMemoryUsageBytes() const {
  size_t size = blocks_.size() * block_size_;
  for (const Allocation& temporary : temporaries_)
    size += temporary.used;
  return size;
}

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBufferV3                                                             |
// +---------------------------------------------------------------------------+

// static
TraceBufferV3::Params TraceBufferV3::DefaultParams(size_t size_in_bytes) {
  Params params{};
  params.staging_bytes =
      std::clamp(size_in_bytes, kMinStagingBytes, kMaxStagingBytes);
  params.bundle_target_bytes =
      std::clamp(size_in_bytes / kBundleTargetDivisor, kMinBundleTargetBytes,
                 kMaxBundleTargetBytes);
  // Aim to hold two bundles per block, with room for the packets that cross
  // the target. Larger bundles can use temporary decode allocations.
  params.decode_block_bytes =
      2 * params.bundle_target_bytes + params.bundle_target_bytes / 4;
  return params;
}

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::Create(size_t size_in_bytes,
                                                     OverwritePolicy policy) {
  return CreateInternal(size_in_bytes, policy, DefaultParams(size_in_bytes));
}

// static
std::unique_ptr<TraceBufferV3> TraceBufferV3::CreateInternal(
    size_t size_in_bytes,
    OverwritePolicy policy,
    const Params& params) {
  const size_t capacity = std::max(size_in_bytes, size_t(1));
  // BundleHeader has 32-bit sizes. A bundle is stored only if it fits the
  // bundled buffer, so this limit makes its bundle_size representable.
  PERFETTO_CHECK(capacity <= std::numeric_limits<uint32_t>::max());
  std::unique_ptr<TraceBufferV3> buf(new TraceBufferV3(params));
  buf->staging_buf_ = TraceBufferV2::Create(params.staging_bytes, policy);
  if (PERFETTO_UNLIKELY(!buf->staging_buf_ ||
                        !buf->bundled_buf_.Initialize(capacity))) {
    return nullptr;
  }
  return buf;
}

TraceBufferV3::TraceBufferV3(const Params& params)
    : params_(params),
      codec_(new internal::ZstdFrameCodec(params.bundle_target_bytes)),
      decode_arena_(params.decode_block_bytes) {}

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
  staging_buf_->CopyChunkUntrusted(producer_id_trusted, client_identity_trusted,
                                   writer_id, chunk_id, num_fragments,
                                   chunk_flags, chunk_complete, src, size);
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
  return staging_buf_->CopyChunkV2Untrusted(sequence, fragments, num_fragments,
                                            first_continues_from_prev,
                                            last_continues_on_next);
}

void TraceBufferV3::RecordChunkV2DataLoss(ProducerID producer_id,
                                          WriterID writer_id) {
  staging_buf_->RecordChunkV2DataLoss(producer_id, writer_id);
}

void TraceBufferV3::RecordAbiViolation() {
  staging_buf_->RecordAbiViolation();
}

void TraceBufferV3::EndWriteBatch() {
  // Stop bundling for kDiscard once a bundle is pending or staging has
  // rejected a write because it is full.
  if (!write_bundling_stopped_ && !staging_buf_->discards_writes()) {
    const uint64_t new_staging_bytes = staging_buf_->stats().bytes_written() -
                                       staging_bytes_written_at_last_drain_;
    if (new_staging_bytes >= params_.bundle_target_bytes)
      BundleAllFromStaging();
  }

  // The previous read batch has been delivered, and bundling may have
  // overwritten its partly read bundle. Keep only bytes still needed by
  // the next read.
  decode_arena_.ResetExcept(oldest_bundle_.records);
}

void TraceBufferV3::BundleAllFromStaging() {
  const base::TimeNanos start = base::GetWallTimeNs();

  staging_buf_->BeginRead();
  bool staging_drained = false;
  while (!staging_drained && !write_bundling_stopped_) {
    staging_drained = BuildBundle() == BuildResult::kStagingDrained;
    FinishOpenBundle();
  }

  // zstd has copied every packet that was read, so the chunks can move.
  staging_buf_->CompactIfWritable();
  RecordBundlingTime(start);
}

void TraceBufferV3::RecordBundlingTime(base::TimeNanos start) {
  const uint64_t duration_us =
      static_cast<uint64_t>((base::GetWallTimeNs() - start).count() / 1000);
  counters_.bundling_max_us = std::max(counters_.bundling_max_us, duration_us);
}

TraceBufferV3::BuildResult TraceBufferV3::BuildBundle() {
  ++open_bundle_.generation;
  open_bundle_.sequence_list.clear();
  open_bundle_.uncompressed_size = 0;
  codec_->BeginFrame();

  // Staging returns the packets of a sequence in runs, so remember the last
  // sequence to skip the map lookup.
  ProducerAndWriterID last_seq_key = 0;
  BundledSequence* bundled_sequence = nullptr;

  for (;;) {
    staged_packet_.Clear();
    PacketSequenceProperties sequence_properties{};
    uint32_t staging_loss_reasons = 0;
    if (!staging_buf_->ReadNextTracePacket(
            &staged_packet_, &sequence_properties, &staging_loss_reasons)) {
      staging_bytes_written_at_last_drain_ =
          staging_buf_->stats().bytes_written();
      return BuildResult::kStagingDrained;
    }
    const ProducerAndWriterID seq_key = MkProducerAndWriterID(
        sequence_properties.producer_id_trusted, sequence_properties.writer_id);

    // SMB v1 can reassemble a packet from empty fragments. Skip it before
    // adding a loss field, just as staging skips empty whole packets.
    if (PERFETTO_UNLIKELY(staged_packet_.size() == 0)) {
      if (staging_loss_reasons)
        staging_buf_->AddSequenceDataLoss(seq_key, staging_loss_reasons);
      continue;
    }

    // Validate before adding the loss field. Appending a field can turn a
    // truncated packet into one that passes validation.
    uint32_t packet_loss_reasons = 0;
    if (PERFETTO_UNLIKELY(!PacketStreamValidator::Validate(
            staged_packet_.slices(), &packet_loss_reasons))) {
      ++counters_.packets_dropped_by_validation;
      // The read cleared the loss in staging. Put it back, so that the next
      // valid packet of the sequence reports it.
      if (staging_loss_reasons)
        staging_buf_->AddSequenceDataLoss(seq_key, staging_loss_reasons);
      continue;
    }

    if (!bundled_sequence || seq_key != last_seq_key) {
      bundled_sequence = AddSequenceToOpenBundle(
          seq_key, sequence_properties.client_identity_trusted);
      last_seq_key = seq_key;
    }

    // The producer can have set previous_packet_dropped too. A parser keeps
    // only the last value of the field, so the field that we append must have
    // both sets of bits.
    uint8_t loss_field[proto_utils::kMaxSimpleFieldEncodedSize];
    size_t loss_field_size = 0;
    if (staging_loss_reasons) {
      uint8_t* end = proto_utils::WriteVarInt(
          proto_utils::MakeTagVarInt(
              protos::pbzero::TracePacket::kPreviousPacketDroppedFieldNumber),
          loss_field);
      end = proto_utils::WriteVarInt(staging_loss_reasons | packet_loss_reasons,
                                     end);
      loss_field_size = static_cast<size_t>(end - loss_field);
    }
    bundled_sequence->loss_reasons_in_open_bundle |=
        staging_loss_reasons | packet_loss_reasons;

    const size_t packet_size = staged_packet_.size() + loss_field_size;
    uint8_t record_header[2 * kMaxVarIntSize];
    const size_t record_header_size = WriteBundleRecordHeader(
        bundled_sequence->index_in_open_bundle, packet_size, record_header);

    // The slices point into staging chunks. The codec consumes or copies them
    // here, before the next staging read or compaction can change the chunks.
    codec_->AddToFrame(record_header, record_header_size);
    for (const Slice& slice : staged_packet_.slices())
      codec_->AddToFrame(slice.start, slice.size);
    codec_->AddToFrame(loss_field, loss_field_size);

    // The target is soft. The packet that crosses it is already in the frame.
    open_bundle_.uncompressed_size += record_header_size + packet_size;
    if (open_bundle_.uncompressed_size >= params_.bundle_target_bytes)
      return BuildResult::kTargetReached;
  }
}

TraceBufferV3::BundledSequence* TraceBufferV3::AddSequenceToOpenBundle(
    ProducerAndWriterID seq_key,
    const ClientIdentity& client_identity) {
  auto [it, inserted] = sequences_.try_emplace(seq_key);
  BundledSequence& bundled_sequence = it->second;
  if (inserted) {
    bundled_sequence.client_identity = client_identity;
    staging_buf_->ProtectSequencePruning(seq_key);
  }
  if (bundled_sequence.open_bundle_generation != open_bundle_.generation) {
    // The first packet of the sequence in the open bundle.
    bundled_sequence.open_bundle_generation = open_bundle_.generation;
    bundled_sequence.index_in_open_bundle =
        static_cast<uint32_t>(open_bundle_.sequence_list.size());
    bundled_sequence.loss_reasons_in_open_bundle = 0;
    open_bundle_.sequence_list.push_back(seq_key);
    ++bundled_sequence.bundle_refs;
  }
  return &bundled_sequence;
}

void TraceBufferV3::FinishOpenBundle() {
  if (open_bundle_.sequence_list.empty())
    return;  // Staging had no valid packet.

  const protozero::ConstBytes frame = codec_->EndFrame();
  const size_t sequence_list_size =
      open_bundle_.sequence_list.size() * sizeof(ProducerAndWriterID);
  // Add in 64 bits so a large frame cannot wrap the size on a 32-bit build.
  const uint64_t bundle_size =
      uint64_t{sizeof(BundleHeader)} + sequence_list_size + frame.size;

  // Drop a bundle that does not fit the whole bundled buffer, in either
  // policy, because removing older bundles would not make it fit.
  // If the bundle fits, |bundle_size| and the number of sequences fit in 32
  // bits. |uncompressed_size| has no such bound, so check it too.
  if (PERFETTO_UNLIKELY(bundle_size > bundled_buf_.capacity() ||
                        open_bundle_.uncompressed_size >
                            std::numeric_limits<uint32_t>::max())) {
    for (ProducerAndWriterID seq_key : open_bundle_.sequence_list) {
      auto it = sequences_.find(seq_key);
      // The packets left staging already. Report their loss, and the loss
      // that they carried, on the next packet that leaves staging.
      // Using |unreported_loss_reasons| would report the loss on the next
      // packet the consumer reads, which can be older than the dropped bundle.
      staging_buf_->AddSequenceDataLoss(
          seq_key, DataLossReason::DATA_LOSS_PRESENT |
                       it->second.loss_reasons_in_open_bundle);
      DropBundleRef(it);
    }
    ++counters_.bundles_too_large;
    return;
  }

  BundleHeader header{};
  header.bundle_size = static_cast<uint32_t>(bundle_size);
  header.uncompressed_size =
      static_cast<uint32_t>(open_bundle_.uncompressed_size);
  header.num_sequences =
      static_cast<uint32_t>(open_bundle_.sequence_list.size());

  ++counters_.bundles_written;
  counters_.bundle_bytes_uncompressed += header.uncompressed_size;
  counters_.bundle_bytes_compressed += bundle_size;
  max_bundle_uncompressed_size_ =
      std::max(max_bundle_uncompressed_size_, header.uncompressed_size);

  if (bundle_size > bundled_buf_.free_size() &&
      overwrite_policy() == kDiscard) {
    // The bundle fits the buffer but not its free space, and a kDiscard
    // buffer keeps its older bundles. The packets left staging already, so
    // keep the bundle aside. This is not a loss.
    pending_bundle_.reset(new uint8_t[header.bundle_size]);
    uint8_t* dst = pending_bundle_.get();
    memcpy(dst, &header, sizeof(header));
    memcpy(dst + sizeof(header), open_bundle_.sequence_list.data(),
           sequence_list_size);
    memcpy(dst + sizeof(header) + sequence_list_size, frame.data, frame.size);
    write_bundling_stopped_ = true;
    return;
  }

  // A kOverwrite buffer removes its oldest bundles until the new one fits.
  while (bundle_size > bundled_buf_.free_size())
    RemoveOldestBundle(RemoveReason::kOverwritten);

  bundled_buf_.Append(&header, sizeof(header));
  bundled_buf_.Append(open_bundle_.sequence_list.data(), sequence_list_size);
  bundled_buf_.Append(frame.data, frame.size);
}

void TraceBufferV3::BeginRead() {
  decode_arena_.ResetExcept(oldest_bundle_.records);
}

bool TraceBufferV3::ReadNextTracePacket(
    TracePacket* packet,
    PacketSequenceProperties* sequence_properties,
    uint32_t* previous_packet_on_sequence_dropped) {
  *sequence_properties = {0, ClientIdentity(), 0};
  *previous_packet_on_sequence_dropped = 0;

  for (;;) {
    if (!oldest_bundle_.is_decoded()) {
      if (bundled_buf_.empty()) {
        // All stored bundles have been read. Read the pending bundle next
        // because its packets are older than those in staging. Then build one
        // from staging and read all of it before building another.
        if (pending_bundle_) {
          bundled_buf_.Append(pending_bundle_.get(), PendingBundleSize());
          pending_bundle_.reset();
        } else if (!BundleOneFromStaging()) {
          return false;
        }
        // The new bundle might have been dropped because it was too large.
        continue;
      }
      if (PERFETTO_UNLIKELY(!LoadOldestBundle()))
        continue;
    }

    const std::optional<BundleRecord> maybe_record = ParseBundleRecord(
        oldest_bundle_.records, oldest_bundle_.header.uncompressed_size,
        oldest_bundle_.read_offset, oldest_bundle_.resolved_sequences.size());
    if (PERFETTO_UNLIKELY(!maybe_record)) {
      RemoveOldestBundle(RemoveReason::kCorrupted);
      continue;
    }
    const BundleRecord& record = *maybe_record;
    oldest_bundle_.read_offset += record.record_size;
    counters_.bytes_read += record.record_size;

    const OldestBundle::ResolvedSequence& resolved =
        oldest_bundle_.resolved_sequences[record.sequence_index];
    *sequence_properties = resolved.properties;

    for (size_t offset = 0; offset < record.packet_size;
         offset += kMaxTracePacketSliceSize) {
      packet->AddSlice(
          record.packet + offset,
          std::min(kMaxTracePacketSliceSize, record.packet_size - offset));
    }

    if (PERFETTO_UNLIKELY(resolved.state->unreported_loss_reasons)) {
      // The packet can have its own previous_packet_dropped field. The
      // service appends the value that we return and a parser keeps only the
      // last one, so return both sets of bits.
      uint32_t packet_loss_reasons = 0;
      PacketStreamValidator::Validate(packet->slices(), &packet_loss_reasons);
      *previous_packet_on_sequence_dropped =
          resolved.state->unreported_loss_reasons | packet_loss_reasons;
      resolved.state->unreported_loss_reasons = 0;
    }

    // Removing the bundle leaves its decoded bytes alive for the rest of
    // this read batch.
    if (oldest_bundle_.read_offset == oldest_bundle_.header.uncompressed_size)
      RemoveOldestBundle(RemoveReason::kFullyRead);
    return true;
  }
}

bool TraceBufferV3::BundleOneFromStaging() {
  PERFETTO_DCHECK(bundled_buf_.empty() && !pending_bundle_);
  const base::TimeNanos start = base::GetWallTimeNs();

  staging_buf_->BeginRead();
  const bool staging_drained = BuildBundle() == BuildResult::kStagingDrained;
  const bool has_packets = !open_bundle_.sequence_list.empty();
  FinishOpenBundle();

  // Compact only once staging is drained. Otherwise each later bundle of this
  // read would move the remaining chunks again.
  //
  // Unlike CompactIfWritable() in BundleAllFromStaging(), MaybeCompact() does
  // nothing for kDiscard. Once EndWriteBatch() stops bundling, staging must
  // fill up and stop. Compacting it after a read would give it room again.
  if (staging_drained)
    staging_buf_->MaybeCompact();
  RecordBundlingTime(start);
  return has_packets;
}

TraceBufferV3::BundleHeader TraceBufferV3::ReadOldestBundleMetadata(
    std::vector<ProducerAndWriterID>* sequence_list) const {
  BundleHeader header{};
  bundled_buf_.CopyOut(0, sizeof(header), &header);

  // Without these sizes we cannot find the next bundle, and nothing else in
  // the buffer can be trusted either.
  PERFETTO_CHECK(header.bundle_size >= sizeof(header) &&
                 header.bundle_size <= bundled_buf_.used_size());
  PERFETTO_CHECK(header.num_sequences <= (header.bundle_size - sizeof(header)) /
                                             sizeof(ProducerAndWriterID));

  sequence_list->resize(header.num_sequences);
  bundled_buf_.CopyOut(sizeof(header),
                       header.num_sequences * sizeof(ProducerAndWriterID),
                       sequence_list->data());
  return header;
}

bool TraceBufferV3::DecompressOldestFrame(const BundleHeader& oldest_header,
                                          uint8_t* dst) {
  const size_t frame_offset =
      sizeof(oldest_header) +
      oldest_header.num_sequences * sizeof(ProducerAndWriterID);
  const size_t frame_size = oldest_header.bundle_size - frame_offset;
  const uint8_t* frame = bundled_buf_.PeekContiguous(frame_offset, frame_size);

  // If the frame wraps, copy it into contiguous memory for zstd.
  std::unique_ptr<uint8_t[]> contiguous_frame;
  if (!frame) {
    contiguous_frame.reset(new uint8_t[frame_size]);
    bundled_buf_.CopyOut(frame_offset, frame_size, contiguous_frame.get());
    frame = contiguous_frame.get();
  }
  return codec_->Decompress(protozero::ConstBytes{frame, frame_size}, dst,
                            oldest_header.uncompressed_size);
}

bool TraceBufferV3::LoadOldestBundle() {
  oldest_bundle_.header =
      ReadOldestBundleMetadata(&oldest_bundle_.sequence_list);

  // Check the size before allocating memory for it.
  bool valid =
      oldest_bundle_.header.uncompressed_size <=
          max_bundle_uncompressed_size_ &&
      oldest_bundle_.read_offset < oldest_bundle_.header.uncompressed_size;

  oldest_bundle_.resolved_sequences.clear();
  oldest_bundle_.resolved_sequences.reserve(
      oldest_bundle_.sequence_list.size());
  for (ProducerAndWriterID seq_key : oldest_bundle_.sequence_list) {
    auto it = sequences_.find(seq_key);
    if (PERFETTO_UNLIKELY(it == sequences_.end())) {
      valid = false;
      break;
    }
    OldestBundle::ResolvedSequence resolved{};
    GetProducerAndWriterID(seq_key, &resolved.properties.producer_id_trusted,
                           &resolved.properties.writer_id);
    resolved.properties.client_identity_trusted = it->second.client_identity;
    resolved.state = &it->second;
    oldest_bundle_.resolved_sequences.push_back(resolved);
  }

  uint8_t* records = nullptr;
  if (valid) {
    records = decode_arena_.Allocate(oldest_bundle_.header.uncompressed_size);
    valid = DecompressOldestFrame(oldest_bundle_.header, records);
  }
  if (PERFETTO_UNLIKELY(!valid)) {
    RemoveOldestBundle(RemoveReason::kCorrupted);
    return false;
  }
  oldest_bundle_.records = records;
  return true;
}

void TraceBufferV3::RemoveOldestBundle(RemoveReason reason) {
  // A decoded bundle has its header and sequences in |oldest_bundle_| already.
  const BundleHeader header =
      oldest_bundle_.is_decoded()
          ? oldest_bundle_.header
          : ReadOldestBundleMetadata(&oldest_bundle_.sequence_list);

  // The loss to mark on every sequence of the bundle. Reading a bundle loses
  // no packets. For an overwrite, the record walk in
  // ProcessOverwrittenBundle() can restrict the loss to the sequences that
  // still have unread packets.
  uint32_t loss_reasons_for_all = 0;
  switch (reason) {
    case RemoveReason::kFullyRead:
      break;
    case RemoveReason::kOverwritten:
      ++counters_.bundles_overwritten;
      loss_reasons_for_all = ProcessOverwrittenBundle(header);
      break;
    case RemoveReason::kCorrupted:
      ++counters_.bundles_corrupted;
      loss_reasons_for_all = kCorruptedLossReasons;
      PERFETTO_ELOG("TraceBufferV3: dropping a corrupted bundle of %" PRIu32
                    " bytes",
                    header.bundle_size);
      break;
  }

  for (ProducerAndWriterID seq_key : oldest_bundle_.sequence_list) {
    auto it = sequences_.find(seq_key);
    if (it == sequences_.end())
      continue;  // Only for a corrupted bundle.
    it->second.unreported_loss_reasons |= loss_reasons_for_all;
    DropBundleRef(it);
  }

  bundled_buf_.Consume(header.bundle_size);
  oldest_bundle_.read_offset = 0;
  oldest_bundle_.records = nullptr;
}

uint32_t TraceBufferV3::ProcessOverwrittenBundle(
    const BundleHeader& oldest_header) {
  const bool has_protovm =
      !protovms_.empty() &&
      std::any_of(oldest_bundle_.sequence_list.begin(),
                  oldest_bundle_.sequence_list.end(),
                  [this](ProducerAndWriterID seq_key) {
                    ProducerID producer_id = 0;
                    WriterID writer_id = 0;
                    GetProducerAndWriterID(seq_key, &producer_id, &writer_id);
                    return protovms_.HasProducer(producer_id);
                  });

  const uint8_t* records = oldest_bundle_.records;
  std::unique_ptr<uint8_t[]> overwritten_records;
  if (!records) {
    // A wholly unread bundle only needs decoding if a ProtoVM wants its
    // packets. Otherwise the sequence list is enough to report the loss.
    if (!has_protovm ||
        oldest_header.uncompressed_size > max_bundle_uncompressed_size_) {
      return kOverwriteLossReasons;
    }
    overwritten_records.reset(new uint8_t[oldest_header.uncompressed_size]);
    if (!DecompressOldestFrame(oldest_header, overwritten_records.get()))
      return kOverwriteLossReasons;
    records = overwritten_records.get();
  }

  TracePacket packet;
  size_t offset = oldest_bundle_.read_offset;
  while (offset < oldest_header.uncompressed_size) {
    const auto maybe_record =
        ParseBundleRecord(records, oldest_header.uncompressed_size, offset,
                          oldest_bundle_.sequence_list.size());
    // If a record is malformed, we cannot tell which remaining sequences
    // have unread packets. The caller marks loss on all of them.
    if (PERFETTO_UNLIKELY(!maybe_record))
      return kOverwriteLossReasons;
    const BundleRecord& record = *maybe_record;
    offset += record.record_size;

    if (oldest_bundle_.is_decoded()) {
      oldest_bundle_.resolved_sequences[record.sequence_index]
          .state->unreported_loss_reasons |= kOverwriteLossReasons;
    }
    if (has_protovm) {
      ProducerID producer_id = 0;
      WriterID writer_id = 0;
      GetProducerAndWriterID(
          oldest_bundle_.sequence_list[record.sequence_index], &producer_id,
          &writer_id);
      packet.Clear();
      packet.AddSlice(record.packet, record.packet_size);
      protovms_.MaybeProcessOverwrittenPacket(packet, producer_id);
    }
  }
  // Only a partly read bundle has resolved sequences, so only then was the
  // loss marked above. A wholly unread bundle reports it through its sequence
  // list in RemoveOldestBundle().
  return oldest_bundle_.is_decoded() ? 0 : kOverwriteLossReasons;
}

void TraceBufferV3::DropBundleRef(SequenceMap::iterator it) {
  BundledSequence& bundled_sequence = it->second;
  PERFETTO_DCHECK(bundled_sequence.bundle_refs > 0);
  if (--bundled_sequence.bundle_refs > 0)
    return;

  // No bundle refers to the sequence now, so its next packet comes from
  // staging. Move the unreported loss there. Staging then also decides how
  // long to keep the state of a sequence that has no packets left anywhere.
  if (bundled_sequence.unreported_loss_reasons) {
    staging_buf_->AddSequenceDataLoss(it->first,
                                      bundled_sequence.unreported_loss_reasons);
  }
  staging_buf_->AllowSequencePruning(it->first);
  sequences_.erase(it);
}

size_t TraceBufferV3::PendingBundleSize() const {
  PERFETTO_DCHECK(pending_bundle_);
  BundleHeader header{};
  memcpy(&header, pending_bundle_.get(), sizeof(header));
  return header.bundle_size;
}

bool TraceBufferV3::returns_validated_packets() const {
  return true;
}

uint64_t TraceBufferV3::packets_dropped_by_validation() const {
  return counters_.packets_dropped_by_validation;
}

void TraceBufferV3::MaybeCompact() {
  decode_arena_.ResetExcept(oldest_bundle_.records);
  decode_arena_.ReleaseEmptyBlocks();
  staging_buf_->MaybeCompact();
  bundled_buf_.ReleaseUnusedPages();
}

std::unique_ptr<TraceBuffer> TraceBufferV3::CloneReadOnly() const {
  std::unique_ptr<TraceBufferV3> clone(new TraceBufferV3(params_));
  clone->write_bundling_stopped_ = write_bundling_stopped_;

  clone->staging_buf_.reset(
      static_cast<TraceBufferV2*>(staging_buf_->CloneReadOnly().release()));
  if (PERFETTO_UNLIKELY(
          !clone->staging_buf_ ||
          !clone->bundled_buf_.InitializeAsCloneOf(bundled_buf_))) {
    return nullptr;  // An allocation failed.
  }

  if (pending_bundle_) {
    const size_t pending_size = PendingBundleSize();
    clone->pending_bundle_.reset(new uint8_t[pending_size]);
    memcpy(clone->pending_bundle_.get(), pending_bundle_.get(), pending_size);
  }

  clone->sequences_ = sequences_;
  clone->protovms_.CloneReadOnlyFrom(protovms_);

  // The entries of |sequences_| carry generations of this buffer. The
  // bundles that the clone builds from its staging need later ones.
  clone->open_bundle_.generation = open_bundle_.generation;
  clone->staging_bytes_written_at_last_drain_ =
      staging_bytes_written_at_last_drain_;

  // The decoded bytes are not copied. The first read of the clone decodes the
  // oldest bundle again and continues at this offset.
  clone->oldest_bundle_.read_offset = oldest_bundle_.read_offset;
  clone->max_bundle_uncompressed_size_ = max_bundle_uncompressed_size_;

  clone->counters_ = counters_;
  clone->counters_.bytes_read = 0;
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

  // In staging, a read moves packets into bundles. Only the packets that
  // ReadNextTracePacket() returned count as read by the consumer.
  stats_.set_staging_bytes_read(stats_.bytes_read());
  stats_.set_staging_chunks_read(stats_.chunks_read());
  stats_.set_staging_readaheads_succeeded(stats_.readaheads_succeeded());
  stats_.set_staging_readaheads_failed(stats_.readaheads_failed());
  stats_.set_bytes_read(counters_.bytes_read);
  stats_.set_chunks_read(0);
  stats_.set_readaheads_succeeded(0);
  stats_.set_readaheads_failed(0);

  stats_.set_bundles_written(counters_.bundles_written);
  stats_.set_bundle_bytes_uncompressed(counters_.bundle_bytes_uncompressed);
  stats_.set_bundle_bytes_compressed(counters_.bundle_bytes_compressed);
  stats_.set_bundles_overwritten(counters_.bundles_overwritten);
  stats_.set_bundles_too_large(counters_.bundles_too_large);
  stats_.set_bundles_corrupted(counters_.bundles_corrupted);
  stats_.set_bundling_max_us(counters_.bundling_max_us);
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
  if (pending_bundle_)
    total_bytes += PendingBundleSize();
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
