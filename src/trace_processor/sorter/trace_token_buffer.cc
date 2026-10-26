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
#include <tuple>
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
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 24;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  static constexpr uint8_t kMaxExtraCountersBits = 4;
  static constexpr uint8_t kMaxExtraCounters = (1 << kMaxExtraCountersBits) - 1;
  static_assert(TrackEventData::kMaxNumExtraCounters <= kMaxExtraCounters,
                "Counter bits must be able to fit TrackEventData counters");

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
  uint32_t is_inline : 1;
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
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 24;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
  uint32_t is_inline : 1;
};
static_assert(sizeof(TracePacketDataDescriptor) == 8,
              "TracePacketDataDescriptor must be small");
static_assert(alignof(TracePacketDataDescriptor) == 8,
              "TracePacketDataDescriptor must be 8-aligned");

struct alignas(8) FtraceDataDescriptor {
  static constexpr uint8_t kMaxOffsetFromInternedBlobBits = 24;
  static constexpr uint32_t kMaxOffsetFromInternedBlob =
      (1Ul << kMaxOffsetFromInternedBlobBits) - 1;

  uint16_t intern_blob_index;
  uint16_t intern_seq_index;
  uint32_t intern_blob_offset : kMaxOffsetFromInternedBlobBits;
  uint32_t is_inline : 1;
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

uint32_t PaddedSize(size_t size) {
  return static_cast<uint32_t>((size + 7) & ~size_t(7));
}

}  // namespace

template <typename Desc>
std::pair<BumpAllocator::AllocId, uint8_t*> TraceTokenBuffer::AppendCommon(
    Desc& desc,
    const TraceBlobView& packet,
    PacketSequenceStateGeneration* sequence_state,
    uint32_t extra_size) {
  // The packet's bytes are copied in after the descriptor, so nothing here
  // refers back to the memory the packet was read or decompressed into.
  // Packets too big for one allocation get a heap copy of their own instead.
  uint32_t fixed_size = GetAllocSize(desc) + extra_size;
  uint32_t padded = PaddedSize(packet.size());
  bool is_inline = fixed_size + padded <= BumpAllocator::kChunkSize;

  void* raw;
  BumpAllocator::AllocId alloc_id =
      allocator_.Alloc(is_inline ? fixed_size + padded : fixed_size, &raw);
  CurrentChunk& chunk = ChunkFor(alloc_id.chunk_index);

  desc.is_inline = is_inline;
  desc.intern_blob_offset = 0;
  if (is_inline) {
    desc.intern_blob_index = 0;
  } else {
    desc.intern_blob_index = AddTraceBlob(
        chunk.blobs, RefPtr<TraceBlob>(new TraceBlob(
                         TraceBlob::CopyFrom(packet.data(), packet.size()))));
  }
  desc.intern_seq_index = InternSeqState(chunk.seqs, sequence_state);

  uint8_t* ptr = AppendToPtr(static_cast<uint8_t*>(raw), desc);
  uint64_t packet_size = static_cast<uint64_t>(packet.size());
  ptr = AppendToPtr(ptr, packet_size);
  if (is_inline) {
    memcpy(ptr, packet.data(), packet.size());
    ptr += padded;
  }
  return {alloc_id, ptr};
}

template <typename Desc>
std::pair<TracePacketData, uint8_t*> TraceTokenBuffer::ExtractCommon(
    Id id,
    const BumpAllocator::Allocation& allocation,
    Desc* out_desc) {
  auto* ptr = static_cast<uint8_t*>(allocation.data());
  *out_desc = ExtractFromPtr<Desc>(&ptr);
  uint64_t packet_size = ExtractFromPtr<uint64_t>(&ptr);

  InternedIndex interned_index = GetInternedIndex(id.alloc_id);
  TraceBlobView tbv;
  if (out_desc->is_inline) {
    const RefPtr<TraceBlob>& chunk = allocation.chunk_blob();
    tbv = TraceBlobView(chunk, static_cast<size_t>(ptr - chunk->data()),
                        static_cast<uint32_t>(packet_size));
    ptr += PaddedSize(packet_size);
  } else {
    RefPtr<TraceBlob>& blob =
        interned_blobs_.at(interned_index)[out_desc->intern_blob_index];
    tbv = TraceBlobView(std::move(blob), 0, static_cast<size_t>(packet_size));
  }
  // Borrowed: the chunk holds the generation until it drains, and that is
  // only acted on in FreeMemory(), after this event has been parsed.
  PacketSequenceStateGeneration* seq =
      interned_seqs_.at(interned_index)[out_desc->intern_seq_index].get();
  return {TracePacketData{std::move(tbv), seq}, ptr};
}

void TraceTokenBuffer::FreeAllocation(Id id,
                                      const BumpAllocator::Allocation& a) {
  if (allocator_.Free(a)) {
    drained_chunks_.push_back(id.alloc_id.chunk_index);
  }
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

  // The TrackEvent is stored as a slice of the packet when the packet is kept,
  // so it does not need its own blob.
  BumpAllocator::AllocId alloc_id;
  uint8_t* ptr;
  if (keep_track_event_packets_) {
    PERFETTO_CHECK(ted.packet);
    std::tie(alloc_id, ptr) = AppendCommon(
        desc, *ted.packet, ted.sequence_state, 2 * sizeof(uint32_t));
    ptr = AppendToPtr(
        ptr, static_cast<uint32_t>(ted.event.data() - ted.packet->data()));
    ptr = AppendToPtr(ptr, static_cast<uint32_t>(ted.event.size()));
  } else {
    PERFETTO_DCHECK(!ted.packet);
    std::tie(alloc_id, ptr) = AppendCommon(desc, ted.event, ted.sequence_state);
  }

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
  auto [alloc_id, ptr] = AppendCommon(desc, data.packet, data.sequence_state);
  base::ignore_result(ptr);
  return Id{alloc_id};
}

TraceTokenBuffer::Id TraceTokenBuffer::Append(FtraceData data) {
  FtraceDataDescriptor desc;
  desc.has_raw_ts = data.raw_ts != FtraceData::kRawTsUnset;
  desc.insert_ftrace_event = data.insert_ftrace_event;
  desc.parse_event = data.parse_event;

  auto [alloc_id, ptr] = AppendCommon(desc, data.packet, data.sequence_state);

  if (desc.has_raw_ts) {
    ptr = AppendToPtr(ptr, data.raw_ts);
  }
  return Id{alloc_id};
}

template <>
TracePacketData TraceTokenBuffer::Extract<TracePacketData>(Id id) {
  TracePacketDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [data, ptr] = ExtractCommon(id, allocation, &desc);
  base::ignore_result(ptr);
  FreeAllocation(id, allocation);
  return std::move(data);
}

template <>
FtraceData TraceTokenBuffer::Extract<FtraceData>(Id id) {
  FtraceDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [tpd, ptr] = ExtractCommon(id, allocation, &desc);
  FtraceData data{std::move(tpd.packet), tpd.sequence_state,
                  FtraceData::kRawTsUnset};
  if (desc.has_raw_ts) {
    data.raw_ts = ExtractFromPtr<int64_t>(&ptr);
  }
  data.insert_ftrace_event = desc.insert_ftrace_event;
  data.parse_event = desc.parse_event;
  FreeAllocation(id, allocation);
  return data;
}

template <>
TrackEventData TraceTokenBuffer::Extract<TrackEventData>(Id id) {
  TrackEventDataDescriptor desc;
  auto allocation = allocator_.GetAllocation(id.alloc_id);
  auto [tpd, ptr] = ExtractCommon(id, allocation, &desc);
  std::optional<TraceBlobView> packet;
  TraceBlobView event;
  if (keep_track_event_packets_) {
    auto offset = ExtractFromPtr<uint32_t>(&ptr);
    auto size = ExtractFromPtr<uint32_t>(&ptr);
    event = tpd.packet.slice_off(offset, size);
    packet = std::move(tpd.packet);
  } else {
    event = std::move(tpd.packet);
  }
  TrackEventData ted{std::move(event), tpd.sequence_state, std::move(packet)};
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
  FreeAllocation(id, allocation);
  return ted;
}

uint16_t TraceTokenBuffer::InternSeqState(SequenceStates* states,
                                          PacketSequenceStateGeneration* ptr) {
  // A chunk holds one reference per generation its events use, taken the
  // first time it sees one and dropped when the chunk drains, so events
  // themselves never touch the refcount. Look back at most 32 elements: more
  // only happens when round-robining between >32 sequences or churning
  // through generations, and then we just store a duplicate.
  size_t lookback = std::min<size_t>(32u, states->size());
  for (uint32_t i = 0; i < lookback; ++i) {
    uint16_t idx = static_cast<uint16_t>(states->size() - 1 - i);
    if ((*states)[idx].get() == ptr) {
      return idx;
    }
  }
  states->emplace_back(ptr);
  PERFETTO_CHECK(states->size() <= std::numeric_limits<uint16_t>::max());
  return static_cast<uint16_t>(states->size() - 1);
}

uint16_t TraceTokenBuffer::AddTraceBlob(Blobs* blobs, RefPtr<TraceBlob> blob) {
  blobs->emplace_back(std::move(blob));
  PERFETTO_CHECK(blobs->size() <= std::numeric_limits<uint16_t>::max());
  return static_cast<uint16_t>(blobs->size() - 1);
}

void TraceTokenBuffer::FreeMemory() {
  // Drop the generations and memory held by chunks which drained since the
  // last call, whether or not the chunks can be erased yet: a straggler in an
  // earlier chunk must not keep every later chunk's generations and memory
  // alive.
  uint64_t first = allocator_.erased_front_chunks_count();
  for (uint64_t chunk_index : drained_chunks_) {
    if (chunk_index < first) {
      continue;
    }
    uint64_t index = chunk_index - first;
    PERFETTO_DCHECK(index < interned_seqs_.size());
    if (!allocator_.HasUnfreed(chunk_index)) {
      interned_seqs_.at(static_cast<size_t>(index)).clear();
    }
    allocator_.ReleaseIfDrained(chunk_index);
  }
  drained_chunks_.clear();

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
