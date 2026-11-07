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

// Create() divides the buffer capacity by this value to set
// |target_packet_data_size_|, capped at kMaxBundleTargetSize.
constexpr size_t kBundleTargetSizeDivisor = 8;

// ReadNextTracePacket() splits each packet into slices of at most this size,
// because the consumer IPC CHECKs that each slice fits in one IPC buffer.
// Matches TracingServiceImpl::kMaxTracePacketSliceSize.
constexpr size_t kMaxReadSliceSize = 128 * 1024 - 512;

// Appends |packet| with its bundle packet header and any loss field to |codec|.
// Returns their total uncompressed size.
// TODO(sashwinbalaji): An invalid packet can consume the appended loss field
// as payload and pass validation. Revisit if this shows up in field traces.
size_t AppendBundlePacket(uint32_t sequence_index,
                          uint32_t loss_reasons,
                          const TracePacket& packet,
                          ZstdFrameCodec* codec) {
  uint8_t loss_field[proto_utils::kMaxSimpleFieldEncodedSize];
  size_t loss_field_size = 0;
  if (PERFETTO_UNLIKELY(loss_reasons)) {
    uint8_t* end = proto_utils::WriteVarInt(
        proto_utils::MakeTagVarInt(
            protos::pbzero::TracePacket::kPreviousPacketDroppedFieldNumber),
        loss_field);
    end = proto_utils::WriteVarInt(loss_reasons, end);
    loss_field_size = static_cast<size_t>(end - loss_field);
  }

  const size_t packet_size = packet.size() + loss_field_size;
  uint8_t bundle_packet_header[2 * PERFETTO_PB_VARINT_MAX_SIZE_64];
  uint8_t* header_end =
      proto_utils::WriteVarInt(sequence_index, bundle_packet_header);
  header_end = proto_utils::WriteVarInt(packet_size, header_end);
  const size_t bundle_packet_header_size =
      static_cast<size_t>(header_end - bundle_packet_header);

  codec->AddToFrame(bundle_packet_header, bundle_packet_header_size);
  for (const Slice& slice : packet.slices())
    codec->AddToFrame(slice.start, slice.size);
  if (PERFETTO_UNLIKELY(loss_field_size))
    codec->AddToFrame(loss_field, loss_field_size);

  return bundle_packet_header_size + packet_size;
}

// One packet from a decompressed bundle, with its sequence index.
struct BundlePacket {
  uint32_t sequence_index = 0;
  // Points into |packet_data_allocator_|.
  protozero::ConstBytes packet_bytes{};
  // Bytes this packet takes in the packet data, with its header.
  size_t encoded_size = 0;
};

// Reads the header at |offset| in |packet_data| and returns a packet view.
BundlePacket ParseBundlePacket(protozero::ConstBytes packet_data,
                               size_t offset) {
  const uint8_t* const begin = packet_data.data + offset;
  const uint8_t* const end = packet_data.data + packet_data.size;

  uint64_t sequence_index = 0;
  const uint8_t* ptr = proto_utils::ParseVarInt(begin, end, &sequence_index);
  PERFETTO_CHECK(ptr != begin);

  uint64_t packet_size = 0;
  const uint8_t* next = proto_utils::ParseVarInt(ptr, end, &packet_size);
  PERFETTO_CHECK(next != ptr && packet_size <= static_cast<size_t>(end - next));

  BundlePacket bundle_packet;
  bundle_packet.sequence_index = static_cast<uint32_t>(sequence_index);
  bundle_packet.packet_bytes = {next, static_cast<size_t>(packet_size)};
  bundle_packet.encoded_size =
      static_cast<size_t>(next - begin) + bundle_packet.packet_bytes.size;
  return bundle_packet;
}

}  // namespace

namespace internal {

// +---------------------------------------------------------------------------+
// | BundleBuffer                                                              |
// +---------------------------------------------------------------------------+

// PushBack() and PeekFrontHeader() copy this as raw bytes, so it must be
// trivially copyable and have no padding.
static_assert(std::is_trivially_copyable_v<BundleHeader>);
static_assert(sizeof(BundleHeader) == 16);

bool BundleBuffer::Initialize(size_t capacity) {
  PERFETTO_CHECK(capacity > 0 &&
                 capacity <= std::numeric_limits<uint32_t>::max());
  data_ = base::PagedMemory::Allocate(
      capacity, base::PagedMemory::kMayFail | base::PagedMemory::kDontCommit);
  if (PERFETTO_UNLIKELY(!data_.IsValid())) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", capacity);
    return false;
  }
  return true;
}

bool BundleBuffer::CloneFrom(const BundleBuffer& src) {
  PERFETTO_CHECK(empty());
  if (PERFETTO_UNLIKELY(!Initialize(src.capacity())))
    return false;
  const size_t size = src.used_size();
  data_.EnsureCommitted(size);
  src.CopyBytes(/*offset_from_front=*/0, size, data_.Get());
  end_ = size;
  max_written_offset_ = size;
  next_bundle_id_ = src.next_bundle_id_;
  return true;
}

void BundleBuffer::PushBack(uint32_t num_sequences,
                            uint32_t packet_data_size,
                            protozero::ConstBytes compressed_packet_data) {
  const size_t bundle_size = sizeof(BundleHeader) + compressed_packet_data.size;
  PERFETTO_CHECK(bundle_size <= free_size());
  BundleHeader header{};
  header.bundle_size = static_cast<uint32_t>(bundle_size);
  header.bundle_id = next_bundle_id_++;
  header.num_sequences = num_sequences;
  header.packet_data_size = packet_data_size;
  AppendBytes(&header, sizeof(header));
  AppendBytes(compressed_packet_data.data, compressed_packet_data.size);
}

BundleHeader BundleBuffer::PeekFrontHeader() const {
  BundleHeader header{};
  CopyBytes(/*offset_from_front=*/0, sizeof(header), &header);

  // PushBack() writes these sizes, so invalid values mean local corruption.
  // Fail here because we cannot find the next bundle without them.
  PERFETTO_CHECK(sizeof(BundleHeader) <= header.bundle_size &&
                 header.bundle_size <= used_size());
  return header;
}

Slice BundleBuffer::PeekFrontCompressedPacketData() const {
  const BundleHeader header = PeekFrontHeader();
  const size_t compressed_packet_data_size =
      header.bundle_size - sizeof(BundleHeader);
  const size_t mem_offset = OffsetOf(begin_ + sizeof(BundleHeader));

  // zstd needs the compressed packet data in one piece, so if it wraps around
  // the end of |data_|, copy both parts into a new allocation.
  if (PERFETTO_UNLIKELY(compressed_packet_data_size >
                        data_.size() - mem_offset)) {
    Slice compressed_packet_data = Slice::Allocate(compressed_packet_data_size);
    CopyBytes(sizeof(BundleHeader), compressed_packet_data_size,
              compressed_packet_data.own_data());
    return compressed_packet_data;
  }

  return {static_cast<const uint8_t*>(data_.Get()) + mem_offset,
          compressed_packet_data_size};
}

void BundleBuffer::PopFront() {
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
  // TraceBufferV2::MaybeCompact(), into a helper that both share.
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

void BundleBuffer::CopyBytes(size_t offset_from_front,
                             size_t size,
                             void* dst) const {
  PERFETTO_DCHECK(offset_from_front <= used_size() &&
                  size <= used_size() - offset_from_front);
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
    size_t target_packet_data_size,
    std::shared_ptr<ZstdFrameCodec> codec) {
  std::unique_ptr<TraceBufferV3> buf(
      new TraceBufferV3(target_packet_data_size, std::move(codec)));
  buf->staging_buf_ = TraceBufferV2::Create(size_in_bytes, policy);
  if (PERFETTO_UNLIKELY(!buf->staging_buf_ ||
                        !buf->bundle_buf_.Initialize(size_in_bytes))) {
    return nullptr;
  }
  return buf;
}

TraceBufferV3::TraceBufferV3(size_t target_packet_data_size,
                             std::shared_ptr<ZstdFrameCodec> codec)
    : target_packet_data_size_(target_packet_data_size),
      codec_(std::move(codec)),
      // A bundle can overshoot the target by its last packet and header, so
      // twice the target fits it unless that packet is larger than the target.
      packet_data_allocator_(
          std::max(kMaxBundleTargetSize, 2 * target_packet_data_size)) {}

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
  // A chunk can store at most |size| bytes, whatever its fragments hold.
  MaybeFreeStagingSpace(size);
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
  size_t max_payload_size = 0;
  for (size_t i = 0; fragments && i < num_fragments; ++i) {
    // Each fragment also stores its size, in a varint of at most 3 bytes.
    max_payload_size += fragments[i].size + 3;
  }
  MaybeFreeStagingSpace(max_payload_size);

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

void TraceBufferV3::MaybeFreeStagingSpace(size_t max_payload_size) {
  if (PERFETTO_UNLIKELY(
          !discard_writes() &&
          staging_buf_->WriteWouldWrapOrOverwrite(max_payload_size))) {
    // A scrape can copy chunks out of order, so this pass can read past a chunk
    // that is still missing, and staging rejects that chunk when it arrives.
    // We accept the rare loss instead of complicating by waiting for the chunk.
    BundleStagingForWrite();
    v3_stats_.set_staging_space_passes(v3_stats_.staging_space_passes() + 1);
  }
}

void TraceBufferV3::OnWriteCheckpoint() {
  if (PERFETTO_UNLIKELY(discard_writes()))
    return;

  if (staging_buf_->used_size() < target_packet_data_size_)
    return;

  BundleStagingForWrite();
}

void TraceBufferV3::BundleStagingForWrite() {
  const base::TimeNanos start = base::GetWallTimeNs();
  staging_buf_->BeginRead();

  // Under kDiscard, stop this pass once cumulative stored bytes reach
  // |bundle_buf_|'s capacity or a bundle does not fit in the free space.
  while (!discard_writes()) {
    if (!FillNextBundle()) {
      // Compaction moves chunks and invalidates staging's read cursor, so run
      // it only once the read is complete.
      staging_buf_->MaybeCompact();
      break;
    }
  }

  RecordBundlingTime(base::GetWallTimeNs() - start);
}

bool TraceBufferV3::BundleStagingForRead() {
  PERFETTO_DCHECK(bundle_buf_.empty());
  if (staging_read_complete_)
    return false;

  const base::TimeNanos start = base::GetWallTimeNs();

  // Empty packets and oversized bundles can leave |bundle_buf_| empty.
  // Stop once a bundle is stored or |staging_buf_| has no readable packets.
  while (bundle_buf_.empty()) {
    if (!FillNextBundle()) {
      staging_read_complete_ = true;
      staging_buf_->MaybeCompact();
      break;
    }
  }

  RecordBundlingTime(base::GetWallTimeNs() - start);
  return !bundle_buf_.empty();
}

void TraceBufferV3::RecordBundlingTime(base::TimeNanos duration) {
  const uint64_t duration_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
  v3_stats_.set_bundling_max_us(
      std::max(v3_stats_.bundling_max_us(), duration_us));
  bundling_time_us_histogram_.Add(static_cast<HistValue>(duration_us));
}

TraceBufferV3::BundleSequence& TraceBufferV3::GetBundleSequence(
    uint32_t bundle_id,
    uint32_t sequence_index) {
  BundleSequence* entry =
      bundle_sequences_.Find(BundleSequenceKey{bundle_id, sequence_index});
  PERFETTO_CHECK(entry);
  return *entry;
}

bool TraceBufferV3::FillNextBundle() {
  PERFETTO_DCHECK(open_bundle_.sequence_indexes.size() == 0);

  const uint32_t bundle_id = bundle_buf_.next_bundle_id();
  // We expect packets from the same sequence to be adjacent, so cache the last
  // |sequence_index| to avoid a lookup per packet.
  // Staging keeps key 0 for padding, so no real packet can match this.
  ProducerAndWriterID last_sequence_key = 0;
  uint32_t sequence_index = 0;
  bool filled = false;

  for (;;) {
    staged_packet_.Clear();
    PacketSequenceProperties properties{};
    uint32_t loss_reasons = 0;
    if (!staging_buf_->ReadNextTracePacket(&staged_packet_, &properties,
                                           &loss_reasons)) {
      break;
    }

    const ProducerAndWriterID sequence_key = MkProducerAndWriterID(
        properties.producer_id_trusted, properties.writer_id);

    // SMB v1 can reassemble an empty packet from empty fragments.
    // Keep its loss for the next packet on this sequence that leaves staging.
    if (PERFETTO_UNLIKELY(staged_packet_.size() == 0)) {
      if (loss_reasons)
        sequence_states().FindOrDie(sequence_key).AddDataLoss(loss_reasons);
      continue;
    }

    if (sequence_key != last_sequence_key) {
      auto& sequence_indexes = open_bundle_.sequence_indexes;
      if (sequence_indexes.size() == 0)
        codec_->BeginFrame();
      auto [index, inserted] = sequence_indexes.Insert(
          sequence_key, static_cast<uint32_t>(sequence_indexes.size()));
      sequence_index = *index;
      // Take a lease on the sequence for its first packet in this bundle.
      if (inserted) {
        auto& sequence = sequence_states().FindOrDie(sequence_key);
        auto [_, is_new] = bundle_sequences_.Insert(
            BundleSequenceKey{bundle_id, sequence_index},
            BundleSequence{sequence.AcquireLease()});
        PERFETTO_CHECK(is_new);
      }
      last_sequence_key = sequence_key;
    }

    if (PERFETTO_UNLIKELY(loss_reasons))
      GetBundleSequence(bundle_id, sequence_index).loss_reasons |= loss_reasons;

    open_bundle_.packet_data_size += AppendBundlePacket(
        sequence_index, loss_reasons, staged_packet_, codec_.get());

    if (open_bundle_.packet_data_size >= target_packet_data_size_) {
      filled = true;
      break;
    }
  }  // for (;;)

  // |codec_| has copied or compressed the slices, so release |staged_packet_|
  // before compaction can move |staging_buf_|'s memory.
  staged_packet_.Clear();
  CloseOpenBundle();
  return filled;
}

void TraceBufferV3::CloseOpenBundle() {
  auto reset_open_bundle = base::OnScopeExit([this] {
    open_bundle_.sequence_indexes.Clear();
    open_bundle_.packet_data_size = 0;
  });

  const uint32_t num_sequences =
      static_cast<uint32_t>(open_bundle_.sequence_indexes.size());

  // We found no packets to bundle, so no frame is open.
  if (num_sequences == 0)
    return;

  const protozero::ConstBytes compressed_packet_data = codec_->EndFrame();
  const size_t bundle_size =
      sizeof(internal::BundleHeader) + compressed_packet_data.size;

  auto drop_open_bundle = [this, num_sequences] {
    const uint32_t bundle_id = bundle_buf_.next_bundle_id();
    for (uint32_t i = 0; i < num_sequences; ++i) {
      BundleSequence& entry = GetBundleSequence(bundle_id, i);
      // The sequence's later packets are still in staging, so they report this
      // drop and any loss the dropped bundle carried.
      entry.lease->AddDataLoss(DataLossReason::DATA_LOSS_PRESENT |
                               entry.loss_reasons);
      bundle_sequences_.Erase(BundleSequenceKey{bundle_id, i});
    }
  };

  // A bundle too big for even an empty buffer is dropped under either policy,
  // but later, smaller bundles can still be stored.
  if (PERFETTO_UNLIKELY(bundle_size > bundle_buf_.capacity() ||
                        open_bundle_.packet_data_size >
                            std::numeric_limits<uint32_t>::max())) {
    drop_open_bundle();
    v3_stats_.set_bundles_too_large(v3_stats_.bundles_too_large() + 1);
    return;
  }

  if (bundle_size > bundle_buf_.free_size()) {
    if (overwrite_policy() == kDiscard) {
      drop_open_bundle();
      StopAcceptingWrites();
      return;
    }

    while (bundle_size > bundle_buf_.free_size())
      OverwriteFrontBundle();
  }

  const uint32_t packet_data_size =
      static_cast<uint32_t>(open_bundle_.packet_data_size);
  v3_stats_.set_bundles_written(v3_stats_.bundles_written() + 1);
  v3_stats_.set_uncompressed_bytes_written(
      v3_stats_.uncompressed_bytes_written() + packet_data_size);
  v3_stats_.set_compressed_bytes_written(v3_stats_.compressed_bytes_written() +
                                         bundle_size);

  // kDiscard stops once we've stored a buffer's worth of bytes in total, even
  // if reads freed some of that space.
  if (overwrite_policy() == kDiscard &&
      v3_stats_.compressed_bytes_written() >= bundle_buf_.capacity()) {
    StopAcceptingWrites();
  }

  bundle_buf_.PushBack(num_sequences, packet_data_size, compressed_packet_data);
}

void TraceBufferV3::OverwriteFrontBundle() {
  PERFETTO_DCHECK(overwrite_policy() == kOverwrite);
  const internal::BundleHeader header = bundle_buf_.PeekFrontHeader();
  const uint32_t bundle_id = header.bundle_id;

  v3_stats_.set_bundles_overwritten(v3_stats_.bundles_overwritten() + 1);
  v3_stats_.set_uncompressed_bytes_overwritten(
      v3_stats_.uncompressed_bytes_overwritten() + header.packet_data_size);

  // Each sequence's producer, indexed by |sequence_index|.
  std::vector<ProducerID> producer_ids;
  bool any_producer_has_protovm = false;
  if (!protovms_.empty()) {
    producer_ids.reserve(header.num_sequences);
    for (uint32_t i = 0; i < header.num_sequences; ++i) {
      const ProducerID producer_id =
          GetBundleSequence(bundle_id, i).lease->properties.producer_id_trusted;
      producer_ids.push_back(producer_id);
      if (protovms_.HasProducer(producer_id))
        any_producer_has_protovm = true;
    }
  }

  if (any_producer_has_protovm) {
    const internal::BlockAllocator::Allocation packet_data =
        packet_data_allocator_.Allocate(header.packet_data_size);
    DecompressFrontBundle(packet_data.data(), packet_data.size());

    TracePacket packet;
    for (size_t offset = 0; offset < packet_data.size();) {
      const BundlePacket bundle_packet =
          ParseBundlePacket({packet_data.data(), packet_data.size()}, offset);
      offset += bundle_packet.encoded_size;
      PERFETTO_CHECK(bundle_packet.sequence_index < producer_ids.size());
      packet.Clear();
      packet.AddSlice(bundle_packet.packet_bytes.data,
                      bundle_packet.packet_bytes.size);
      protovms_.MaybeProcessOverwrittenPacket(
          packet, producer_ids[bundle_packet.sequence_index]);
    }
  }  // if (any_producer_has_protovm)

  for (uint32_t i = 0; i < header.num_sequences; ++i) {
    BundleSequence& entry = GetBundleSequence(bundle_id, i);
    entry.lease->AddBundleLoss(entry.loss_reasons |
                               DataLossReason::DATA_LOSS_OVERWRITE);
    // Erasing the entry ends its lease, so this loss reaches the sequence's
    // next packet, whether that packet is in a later bundle or in staging.
    bundle_sequences_.Erase(BundleSequenceKey{bundle_id, i});
  }

  bundle_buf_.PopFront();
}

void TraceBufferV3::DecompressFrontBundle(uint8_t* dst, size_t dst_size) {
  const auto compressed_packet_data =
      bundle_buf_.PeekFrontCompressedPacketData();
  const bool decompressed = codec_->Decompress(
      /*frame=*/{static_cast<const uint8_t*>(compressed_packet_data.start),
                 compressed_packet_data.size},
      dst, dst_size);
  PERFETTO_CHECK(decompressed);
}

void TraceBufferV3::BeginRead() {
  // Packets returned by earlier reads are no longer needed.
  returned_packet_data_.clear();
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

  if (!decoded_bundle_.packet_data && !DecodeNextBundle())
    return false;

  const BundlePacket bundle_packet = ParseBundlePacket(
      {decoded_bundle_.packet_data.data(), decoded_bundle_.packet_data.size()},
      decoded_bundle_.read_offset);
  decoded_bundle_.read_offset += bundle_packet.encoded_size;

  // A bad index means local corruption, and indexing |sequences| isn't checked.
  PERFETTO_CHECK(bundle_packet.sequence_index <
                 decoded_bundle_.sequences.size());
  auto& sequence = decoded_bundle_.sequences[bundle_packet.sequence_index];

  for (size_t offset = 0; offset < bundle_packet.packet_bytes.size;
       offset += kMaxReadSliceSize) {
    packet->AddSlice(
        bundle_packet.packet_bytes.data + offset,
        std::min(kMaxReadSliceSize, bundle_packet.packet_bytes.size - offset));
  }

  *sequence_properties = sequence.properties;
  *previous_packet_on_sequence_dropped = sequence.unreported_loss_reasons;
  sequence.unreported_loss_reasons = 0;
  bytes_read_ += bundle_packet.encoded_size;

  // Returned slices still point into this packet data, so it has to stay.
  if (decoded_bundle_.read_offset == decoded_bundle_.packet_data.size()) {
    returned_packet_data_.push_back(std::move(decoded_bundle_.packet_data));
    decoded_bundle_ = DecodedBundle();
  }
  return true;
}

bool TraceBufferV3::DecodeNextBundle() {
  // Drain |bundle_buf_| before reading |staging_buf_|, so bundling can't
  // overwrite older packets still waiting to be read.
  if (bundle_buf_.empty() && !BundleStagingForRead())
    return false;

  const internal::BundleHeader header = bundle_buf_.PeekFrontHeader();

  decoded_bundle_.packet_data =
      packet_data_allocator_.Allocate(header.packet_data_size);
  DecompressFrontBundle(decoded_bundle_.packet_data.data(),
                        decoded_bundle_.packet_data.size());

  const uint32_t bundle_id = header.bundle_id;
  decoded_bundle_.sequences.reserve(header.num_sequences);
  for (uint32_t i = 0; i < header.num_sequences; ++i) {
    internal::SequenceState& state = *GetBundleSequence(bundle_id, i).lease;
    DecodedBundle::Sequence sequence{};
    sequence.properties = state.properties;
    // Save the loss preceding this bundle in |sequence| for its first packet.
    // Writes may overwrite newer bundles while this one is partly read, so
    // those later losses must not be reported on this bundle's earlier packets.
    sequence.unreported_loss_reasons = state.TakeBundleLoss();
    decoded_bundle_.sequences.push_back(sequence);
    // |sequence| now holds all that reads need, so the entry can end.
    bundle_sequences_.Erase(BundleSequenceKey{bundle_id, i});
  }

  // Reads now use |packet_data_allocator_|, so reclaim the compressed bundle's
  // space in |bundle_buf_| even while its packets remain unread.
  bundle_buf_.PopFront();
  return true;
}

void TraceBufferV3::MaybeCompact() {
  // Compaction doesn't raise kDiscard's limit, because writes still stop once
  // |v3_stats_.compressed_bytes_written()| reaches |bundle_buf_|'s capacity.
  staging_buf_->MaybeCompact();
  bundle_buf_.MaybeCompact();
  returned_packet_data_ = decltype(returned_packet_data_)();

  // The maps keep their capacity after a burst of writers or bundles, so free
  // them once they are empty.
  // |open_bundle_| is always empty here, because no bundle is being built.
  open_bundle_.sequence_indexes = decltype(open_bundle_.sequence_indexes)();
  if (bundle_sequences_.size() == 0)
    bundle_sequences_ = decltype(bundle_sequences_)();
}

std::unique_ptr<TraceBuffer> TraceBufferV3::CloneReadOnly() const {
  std::unique_ptr<TraceBufferV3> clone(
      new TraceBufferV3(target_packet_data_size_, codec_));
  clone->staging_buf_.reset(
      static_cast<TraceBufferV2*>(staging_buf_->CloneReadOnly().release()));
  if (PERFETTO_UNLIKELY(!clone->staging_buf_))
    return nullptr;
  if (PERFETTO_UNLIKELY(!clone->bundle_buf_.CloneFrom(bundle_buf_)))
    return nullptr;

  // The cloned map has no leases, so take one per cloned bundle sequence.
  for (auto it = bundle_sequences_.GetIterator(); it; ++it) {
    const BundleSequence& src = it.value();
    const PacketSequenceProperties& properties = src.lease->properties;
    internal::SequenceState& sequence =
        clone->sequence_states().FindOrDie(MkProducerAndWriterID(
            properties.producer_id_trusted, properties.writer_id));
    clone->bundle_sequences_.Insert(
        it.key(), BundleSequence{sequence.AcquireLease(), src.loss_reasons});
  }

  clone->protovms_.CloneReadOnlyFrom(protovms_);
  clone->v3_stats_ = v3_stats_;
  clone->bundling_time_us_histogram_ = bundling_time_us_histogram_;

  DecodedBundle& decoded = clone->decoded_bundle_;
  decoded.sequences = decoded_bundle_.sequences;
  if (decoded_bundle_.packet_data) {
    const size_t read_offset = decoded_bundle_.read_offset;
    const size_t unread_size = decoded_bundle_.packet_data.size() - read_offset;
    decoded.packet_data = clone->packet_data_allocator_.Allocate(unread_size);
    memcpy(decoded.packet_data.data(),
           decoded_bundle_.packet_data.data() + read_offset, unread_size);
  }
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
  total_bytes += packet_data_allocator_.GetMemoryUsageBytes();
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
