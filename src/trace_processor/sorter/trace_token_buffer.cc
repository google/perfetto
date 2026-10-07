/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/sorter/trace_token_buffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto::trace_processor {
namespace {

struct alignas(8) TrackEventDataDescriptor {
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 25;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  static constexpr uint8_t kMaxExtraCountersBits = 4;
  static constexpr uint8_t kMaxExtraCounters = (1 << kMaxExtraCountersBits) - 1;
  static_assert(TrackEventData::kMaxNumExtraCounters <= kMaxExtraCounters,
                "Counter bits must be able to fit TrackEventData counters");

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
  uint32_t has_thread_timestamp : 1;
  uint32_t has_thread_instruction_count : 1;
  uint32_t has_counter_value : 1;
  uint32_t extra_counter_count : kMaxExtraCountersBits;
};
static_assert(sizeof(TrackEventDataDescriptor) == 8,
              "CompressedTracePacketData must be small");
static_assert(alignof(TrackEventDataDescriptor) == 8,
              "CompressedTracePacketData must be 8-aligned");

struct alignas(8) TracePacketDataDescriptor {
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 25;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
};
static_assert(sizeof(TracePacketDataDescriptor) == 8,
              "TracePacketDataDescriptor must be small");
static_assert(alignof(TracePacketDataDescriptor) == 8,
              "TracePacketDataDescriptor must be 8-aligned");

struct alignas(8) FtraceDataDescriptor {
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 25;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
  uint32_t has_raw_ts : 1;
  uint32_t insert_ftrace_event : 1;
  uint32_t parse_event : 1;
};
static_assert(sizeof(FtraceDataDescriptor) == 8,
              "FtraceDataDescriptor must be small");
static_assert(alignof(FtraceDataDescriptor) == 8,
              "FtraceDataDescriptor must be 8-aligned");
static_assert(FtraceDataDescriptor::kMaxOffsetFromInternedBlob ==
                  TrackEventDataDescriptor::kMaxOffsetFromInternedBlob,
              "Descriptors must agree on max blob offset");
static_assert(TracePacketDataDescriptor::kMaxOffsetFromInternedBlob ==
                  TrackEventDataDescriptor::kMaxOffsetFromInternedBlob,
              "Descriptors must agree on max blob offset");

template <typename T>
T ExtractFromPtr(uint8_t** ptr) {
  T* typed_ptr = reinterpret_cast<T*>(*ptr);
  T value(std::move(*typed_ptr));
  typed_ptr->~T();
  *ptr += sizeof(T);
  return value;
}

template <typename T>
uint8_t* AppendToPtr(uint8_t* ptr, T value) {
  new (ptr) T(std::move(value));
  return ptr + sizeof(T);
}

uint32_t GetAllocSize(const TrackEventDataDescriptor& desc) {
  uint32_t alloc_size = sizeof(TrackEventDataDescriptor);
  alloc_size += sizeof(uint64_t);
  alloc_size += desc.has_thread_instruction_count * sizeof(int64_t);
  alloc_size += desc.has_thread_timestamp * sizeof(int64_t);
  alloc_size += desc.has_counter_value * sizeof(double);
  alloc_size += desc.extra_counter_count * sizeof(double);
  return alloc_size;
}

uint32_t GetAllocSize(const FtraceDataDescriptor& desc) {
  uint32_t alloc_size = sizeof(FtraceDataDescriptor);
  alloc_size += sizeof(uint64_t);
  alloc_size += desc.has_raw_ts * sizeof(int64_t);
  return alloc_size;
}

uint32_t GetAllocSize(const TracePacketDataDescriptor&) {
  return sizeof(TracePacketDataDescriptor) + sizeof(uint64_t);
}

}  // namespace

template <typename Desc>
std::pair<BumpAllocator::AllocId, uint8_t*> TraceTokenBuffer::AppendCommon(
    Desc& desc,
    const TraceBlobView& packet,
    RefPtr<PacketSequenceStateGeneration> sequence_state) {
  void* raw;
  BumpAllocator::AllocId alloc_id = allocator_.Alloc(GetAllocSize(desc), &raw);
  CurrentChunk& chunk = ChunkFor(alloc_id.chunk_index);

  desc.intern_blob_offset = InternTraceBlob(*chunk.blobs, packet);
  desc.intern_blob_index = static_cast<uint16_t>(chunk.blobs->size() - 1);
  desc.intern_seq_index =
      InternSeqState(*chunk.seqs, std::move(sequence_state));

  uint8_t* ptr = AppendToPtr(static_cast<uint8_t*>(raw), desc);
  uint64_t packet_size = static_cast<uint64_t>(packet.size());
  ptr = AppendToPtr(ptr, packet_size);
  return {alloc_id, ptr};
}

template <typename Desc>
std::pair<TracePacketData, uint8_t*>
TraceTokenBuffer::ExtractCommon(Id id, Desc* out_desc, uint8_t* ptr) {
  *out_desc = ExtractFromPtr<Desc>(&ptr);
  uint64_t packet_size = ExtractFromPtr<uint64_t>(&ptr);

  InternedIndex interned_index = GetInternedIndex(id.alloc_id);
  BlobWithOffset& bwo =
      interned_blobs_.at(interned_index)[out_desc->intern_blob_index];
  TraceBlobView tbv(RefPtr<TraceBlob>::FromReleasedUnsafe(bwo.blob),
                    bwo.offset_in_blob + out_desc->intern_blob_offset,
                    static_cast<uint32_t>(packet_size));
  auto seq = RefPtr<PacketSequenceStateGeneration>::FromReleasedUnsafe(
      interned_seqs_.at(interned_index)[out_desc->intern_seq_index]);
  return {TracePacketData{std::move(tbv), std::move(seq)}, ptr};
}

TraceTokenBuffer::Id TraceTokenBuffer::Append(TrackEventData&& ted) {
  // TrackEventData (and TracePacketData) are two big contributors to the size
  // of the peak memory usage by sorted. The main reasons for this are a) object
  // padding and b) using more bits than necessary to store their contents.
  //
  // The purpose of this function is to "compress" the contents of
  // TrackEventData by utilising techniques like bitpacking, interning and
  // variable length encoding to ensure only the amount of data which really
  // needs to be stored is done so.

  // Compress all the booleans indicating the presence of a value into 4 bits
  // instead of 4 bytes as they would take inside base::Optional.
  TrackEventDataDescriptor desc;
  desc.has_thread_instruction_count = ted.thread_instruction_count.has_value();
  desc.has_thread_timestamp = ted.thread_timestamp.has_value();
  desc.has_counter_value = std::not_equal_to<double>()(ted.counter_value, 0);
  desc.extra_counter_count = ted.CountExtraCounterValues();

  TracePacketData& tpd = ted.trace_packet_data;
  auto [alloc_id, ptr] =
      AppendCommon(desc, tpd.packet, std::move(tpd.sequence_state));

  if (desc.has_thread_instruction_count) {
    ptr = AppendToPtr(ptr, ted.thread_instruction_count.value());
  }
  if (desc.has_thread_timestamp) {
    ptr = AppendToPtr(ptr, ted.thread_timestamp.value());
  }
  if (desc.has_counter_value) {
    ptr = AppendToPtr(ptr, ted.counter_value);
  }
  for (uint32_t i = 0; i < desc.extra_counter_count; ++i) {
    ptr = AppendToPtr(ptr, ted.extra_counter_values[i]);
  }
  return Id{alloc_id};
}

TraceTokenBuffer::Id TraceTokenBuffer::Append(TracePacketData data) {
  TracePacketDataDescriptor desc;
  auto [alloc_id, ptr] =
      AppendCommon(desc, data.packet, std::move(data.sequence_state));
  base::ignore_result(ptr);
  return Id{alloc_id};
}

TraceTokenBuffer::Id TraceTokenBuffer::Append(FtraceData data) {
  FtraceDataDescriptor desc;
  desc.has_raw_ts = data.raw_ts != FtraceData::kRawTsUnset;
  desc.insert_ftrace_event = data.insert_ftrace_event;
  desc.parse_event = data.parse_event;

  auto [alloc_id, ptr] =
      AppendCommon(desc, data.packet, std::move(data.sequence_state));

  if (desc.has_raw_ts) {
    ptr = AppendToPtr(ptr, data.raw_ts);
  }
  return Id{alloc_id};
}

template <>
TracePacketData TraceTokenBuffer::Extract<TracePacketData>(Id id) {
  TracePacketDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [data, ptr] =
      ExtractCommon(id, &desc, static_cast<uint8_t*>(allocation.data()));
  base::ignore_result(ptr);
  allocator_.Free(allocation);
  return std::move(data);
}

template <>
FtraceData TraceTokenBuffer::Extract<FtraceData>(Id id) {
  FtraceDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [tpd, ptr] =
      ExtractCommon(id, &desc, static_cast<uint8_t*>(allocation.data()));
  FtraceData data{std::move(tpd.packet), std::move(tpd.sequence_state),
                  FtraceData::kRawTsUnset};
  if (desc.has_raw_ts) {
    data.raw_ts = ExtractFromPtr<int64_t>(&ptr);
  }
  data.insert_ftrace_event = desc.insert_ftrace_event;
  data.parse_event = desc.parse_event;
  allocator_.Free(allocation);
  return data;
}

template <>
TrackEventData TraceTokenBuffer::Extract<TrackEventData>(Id id) {
  TrackEventDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [tpd, ptr] =
      ExtractCommon(id, &desc, static_cast<uint8_t*>(allocation.data()));
  TrackEventData ted{std::move(tpd.packet), std::move(tpd.sequence_state)};
  if (desc.has_thread_instruction_count) {
    ted.thread_instruction_count = ExtractFromPtr<int64_t>(&ptr);
  }
  if (desc.has_thread_timestamp) {
    ted.thread_timestamp = ExtractFromPtr<int64_t>(&ptr);
  }
  if (desc.has_counter_value) {
    ted.counter_value = ExtractFromPtr<double>(&ptr);
  }
  for (uint32_t i = 0; i < desc.extra_counter_count; ++i) {
    ted.extra_counter_values[i] = ExtractFromPtr<double>(&ptr);
  }
  allocator_.Free(allocation);
  return ted;
}

uint32_t TraceTokenBuffer::InternTraceBlob(BlobWithOffsets& blobs,
                                           const TraceBlobView& tbv) {
  RefPtr<TraceBlob> blob = tbv.blob();
  size_t offset = tbv.offset();
  if (blobs.empty()) {
    return AddTraceBlob(blobs, std::move(blob), offset);
  }

  BlobWithOffset& last_blob = blobs.back();
  if (last_blob.blob != blob.get()) {
    return AddTraceBlob(blobs, std::move(blob), offset);
  }

  // Offsets can go backwards when deferred packets (e.g., those waiting for
  // clock snapshots) are pushed after regular packets. In that case, we just
  // add a new blob entry rather than relying on the relative offset.
  if (last_blob.offset_in_blob > offset) {
    return AddTraceBlob(blobs, std::move(blob), offset);
  }

  // To allow our offsets in the store to be 16 bits, we intern not only the
  // TraceBlob pointer but also the offset. By having this double indirection,
  // we can store offset always as uint16 at the cost of storing blobs here more
  // often: this more than pays for itself as in the majority of cases the
  // offsets are small anyway.
  size_t rel_offset = offset - last_blob.offset_in_blob;
  if (rel_offset > TrackEventDataDescriptor::kMaxOffsetFromInternedBlob) {
    return AddTraceBlob(blobs, std::move(blob), offset);
  }

  // Intentionally "leak" this pointer. This essentially keeps the refcount
  // of this TraceBlob one higher than the number of RefPtrs pointing to it.
  // This allows avoid storing the same RefPtr n times.
  //
  // Calls to this function are paired to the matching Extract<T> which picks
  // up this "leaked" pointer.
  TraceBlob* leaked = blob.ReleaseUnsafe();
  base::ignore_result(leaked);
  return static_cast<uint32_t>(rel_offset);
}

uint16_t TraceTokenBuffer::InternSeqState(
    SequenceStates& states,
    RefPtr<PacketSequenceStateGeneration> ptr) {
  // Look back at most 32 elements. This should be far enough in most cases
  // unless either: a) we are essentially round-robining between >32 sequences
  // b) we are churning through generations. Either case seems pathological.
  size_t lookback = std::min<size_t>(32u, states.size());
  for (uint32_t i = 0; i < lookback; ++i) {
    uint16_t idx = static_cast<uint16_t>(states.size() - 1 - i);
    if (states[idx] == ptr.get()) {
      // Intentionally "leak" this pointer. See |InternTraceBlob| for an
      // explanation.
      PacketSequenceStateGeneration* leaked = ptr.ReleaseUnsafe();
      base::ignore_result(leaked);
      return idx;
    }
  }
  states.emplace_back(ptr.ReleaseUnsafe());
  PERFETTO_CHECK(states.size() <= std::numeric_limits<uint16_t>::max());
  return static_cast<uint16_t>(states.size() - 1);
}

uint32_t TraceTokenBuffer::AddTraceBlob(BlobWithOffsets& blobs,
                                        RefPtr<TraceBlob> blob,
                                        size_t offset) {
  blobs.emplace_back(BlobWithOffset{blob.ReleaseUnsafe(), offset});
  PERFETTO_CHECK(blobs.size() <= std::numeric_limits<uint16_t>::max());
  return 0u;
}

void TraceTokenBuffer::FreeMemory() {
  uint64_t erased = allocator_.EraseFrontFreeChunks();
  if (erased == 0) {
    return;
  }
  PERFETTO_CHECK(erased <= std::numeric_limits<size_t>::max());
  interned_blobs_.erase_front(static_cast<size_t>(erased));
  interned_seqs_.erase_front(static_cast<size_t>(erased));
  PERFETTO_CHECK(interned_blobs_.size() == interned_seqs_.size());
  // The queues changed shape so the cached chunk pointers are stale.
  current_chunk_ = CurrentChunk();
}

void TraceTokenBuffer::SwitchChunk(uint64_t chunk_index) {
  uint64_t index = chunk_index - allocator_.erased_front_chunks_count();
  PERFETTO_DCHECK(interned_blobs_.size() == interned_seqs_.size());
  // The allocator opens at most one chunk per allocation.
  PERFETTO_DCHECK(index <= interned_blobs_.size());
  if (index == interned_blobs_.size()) {
    interned_blobs_.emplace_back();
    interned_seqs_.emplace_back();
  }
  current_chunk_.chunk_index = chunk_index;
  current_chunk_.blobs = &interned_blobs_.at(static_cast<size_t>(index));
  current_chunk_.seqs = &interned_seqs_.at(static_cast<size_t>(index));
}

TraceTokenBuffer::InternedIndex TraceTokenBuffer::GetInternedIndex(
    BumpAllocator::AllocId alloc_id) {
  uint64_t interned_index =
      alloc_id.chunk_index - allocator_.erased_front_chunks_count();
  PERFETTO_DCHECK(interned_index <= std::numeric_limits<size_t>::max());
  PERFETTO_DCHECK(interned_index < interned_blobs_.size());
  PERFETTO_DCHECK(interned_index < interned_seqs_.size());
  PERFETTO_DCHECK(interned_blobs_.size() == interned_seqs_.size());
  return static_cast<size_t>(interned_index);
}

}  // namespace perfetto::trace_processor
