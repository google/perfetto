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

#ifndef SRC_TRACE_PROCESSOR_SORTER_TRACE_TOKEN_BUFFER_H_
#define SRC_TRACE_PROCESSOR_SORTER_TRACE_TOKEN_BUFFER_H_

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/util/bump_allocator.h"
#include "src/trace_processor/util/chunk_memory.h"

namespace perfetto {
namespace trace_processor {

// Helper class which stores tokenized objects while the corresponding events
// are being sorted by TraceSorter.
//
// This class intrusively compresses the tokenized objects as much as possible
// to reduce their memory footprint. This is important to reduce the peak memory
// usage of TraceProcessor which is always hit at some point during sorting.
// The tokenized objects make up the vast majority of this peak so we trade the
// complexity in this class for big reductions in the peak use.
//
// go/perfetto-tp-memory-use gives an overview of trace processor memory usage.
class TraceTokenBuffer {
 public:
  // |keep_track_event_packets| selects whether TrackEventData carries its whole
  // TracePacket in addition to the TrackEvent; it must match what the
  // tokenizer produces for the whole trace.
  TraceTokenBuffer(bool keep_track_event_packets, RefPtr<ChunkMemory> memory)
      : keep_track_event_packets_(keep_track_event_packets),
        allocator_(std::move(memory)) {}
  // Keeps every chunk on the heap.
  explicit TraceTokenBuffer(bool keep_track_event_packets)
      : TraceTokenBuffer(keep_track_event_packets,
                         ChunkMemory::Heap(BumpAllocator::kChunkSize)) {}

  // Identifier returned when appending items to this buffer. This id can
  // later by passed to |Extract| to retrieve the event.
  struct Id {
    // The allocation id of the object in the buffer.
    BumpAllocator::AllocId alloc_id;
  };

  // Appends an object of type |T| to the token buffer. Returns an id for
  // looking up the object later using |Extract|.
  template <typename T>
  PERFETTO_WARN_UNUSED_RESULT Id Append(T object) {
    static_assert(sizeof(T) % 8 == 0, "Size must be a multiple of 8");
    static_assert(alignof(T) == 8, "Alignment must be 8");
    void* ptr;
    BumpAllocator::AllocId id = allocator_.Alloc(sizeof(T), &ptr);
    ChunkFor(id.chunk_index);
    new (ptr) T(std::move(object));
    // Paging moves the bytes of an object to a new address, which only types
    // without pointers into themselves survive.
    if constexpr (!std::is_trivially_copyable_v<T>) {
      allocator_.PinLastChunk();
    }
    return Id{id};
  }
  PERFETTO_WARN_UNUSED_RESULT Id Append(TrackEventData&&);

  // Writes a track event record in place, extracted as a TrackEventData with
  // |is_record| set: BeginTrackEventRecord returns where to write up to
  // |max_size| bytes, or null if that cannot be written in place. Exactly one
  // of CommitTrackEventRecord, with the record's size, and
  // AbortTrackEventRecord must follow, with nothing appended in between.
  uint8_t* BeginTrackEventRecord(size_t max_size);
  PERFETTO_WARN_UNUSED_RESULT Id
  CommitTrackEventRecord(size_t size, PacketSequenceStateGeneration*);
  void AbortTrackEventRecord();
  PERFETTO_WARN_UNUSED_RESULT Id Append(FtraceData);
  PERFETTO_WARN_UNUSED_RESULT Id Append(TracePacketData);

  // Gets a pointer an object of type |T| from the token buffer using an id
  // previously returned by |Append|. This type *must* match the type added
  // using Append. Mismatching types will caused undefined behaviour.
  template <typename T>
  PERFETTO_WARN_UNUSED_RESULT T* Get(Id id) {
    return static_cast<T*>(allocator_.GetPointer(id.alloc_id));
  }

  // Extracts an object of type |T| from the token buffer using an id previously
  // returned by |Append|. This type *must* match the type added using Append.
  // Mismatching types will caused undefined behaviour.
  template <typename T>
  PERFETTO_WARN_UNUSED_RESULT T Extract(Id id) {
    BumpAllocator::Allocation allocation =
        allocator_.GetAllocation(id.alloc_id);
    T* typed_ptr = static_cast<T*>(allocation.data());
    T object(std::move(*typed_ptr));
    typed_ptr->~T();
    FreeAllocation(id, allocation);
    return object;
  }

  // Returns the "past-the-end" id from the underlying allocator.
  // The main use of this function is to provide an id which is greater than
  // all ids previously returned by |Append|.
  //
  // This is similar to the |end()| function in standard library vector classes.
  BumpAllocator::AllocId PastTheEndAllocId() {
    return allocator_.PastTheEndId();
  }

  // Attempts to free any memory retained by this buffer and the underlying
  // allocator. The amount of memory free is implementation defined.
  void FreeMemory();

  // Whether an allocation freed since the last FreeMemory() drained its
  // chunk, so FreeMemory() has memory to give back.
  bool has_drained_chunks() const { return !drained_chunks_.empty(); }

 private:
  using InternedIndex = size_t;
  // The heap copies of the packets in a chunk which are too big to inline.
  using Blobs = std::vector<RefPtr<TraceBlob>>;
  using SequenceStates = std::vector<RefPtr<PacketSequenceStateGeneration>>;

  // Adds |blob| to |blobs| and returns its index.
  uint16_t AddTraceBlob(Blobs* blobs, RefPtr<TraceBlob> blob);
  // Interns PacketSequenceStateGeneration: as these are shared between most
  // packets of a sequence, storing them once per chunk significantly reduces
  // memory use.
  uint16_t InternSeqState(SequenceStates* states,
                          PacketSequenceStateGeneration* ptr);

  // Nearly every append lands in the chunk of the previous one, so keep that
  // chunk's intern vectors at hand instead of walking the queues each time.
  // Cleared whenever the queues change shape.
  struct CurrentChunk {
    uint64_t chunk_index = std::numeric_limits<uint64_t>::max();
    Blobs* blobs = nullptr;
    SequenceStates* seqs = nullptr;
  };
  // Returns the intern vectors of the allocator chunk |chunk_index|, adding
  // them if the allocator has just opened that chunk. Every allocation must
  // pass through here to keep the vectors in step with the chunks.
  CurrentChunk& ChunkFor(uint64_t chunk_index) {
    if (PERFETTO_UNLIKELY(current_chunk_.chunk_index != chunk_index)) {
      SwitchChunk(chunk_index);
    }
    return current_chunk_;
  }
  void SwitchChunk(uint64_t chunk_index);

  // Common prologue shared between Append(TrackEventData|TracePacketData|
  // FtraceData): allocates storage sized for |desc|, fills its intern_* fields,
  // writes |desc| and the packet size to the buffer, and returns the alloc id
  // plus a pointer positioned after the packet size (where any per-type
  // optional fields can be appended).
  template <typename Desc>
  std::pair<BumpAllocator::AllocId, uint8_t*> AppendCommon(
      Desc& desc,
      const TraceBlobView& packet,
      PacketSequenceStateGeneration* sequence_state,
      uint32_t extra_size = 0);

  // Common prologue shared between Extract<TrackEventData|TracePacketData|
  // FtraceData>: reads |*out_desc| and the packet size out of the buffer,
  // materialises the TraceBlobView and sequence state, and returns them as a
  // TracePacketData together with a pointer positioned after the packet size
  // (where any per-type optional fields live). The caller is responsible for
  // freeing the allocation.
  template <typename Desc>
  std::pair<TracePacketData, uint8_t*> ExtractCommon(
      Id id,
      const BumpAllocator::Allocation& allocation,
      Desc* out_desc);

  // Frees |a|, noting its chunk for FreeMemory() if that drained it.
  void FreeAllocation(Id id, const BumpAllocator::Allocation& a);

  InternedIndex GetInternedIndex(BumpAllocator::AllocId);

  const bool keep_track_event_packets_;
  BumpAllocator allocator_;
  base::CircularQueue<Blobs> interned_blobs_;
  base::CircularQueue<SequenceStates> interned_seqs_;
  CurrentChunk current_chunk_;
  // The record between BeginTrackEventRecord and its Commit or Abort.
  BumpAllocator::AllocId pending_record_{};
  uint8_t* pending_record_ptr_ = nullptr;
  uint32_t pending_record_size_ = 0;
  // Chunks whose last allocation was freed since the last FreeMemory().
  std::vector<uint64_t> drained_chunks_;
};

// GCC7 does not like us declaring these inside the class so define these
// out-of-line.
template <>
PERFETTO_WARN_UNUSED_RESULT TrackEventData
    TraceTokenBuffer::Extract<TrackEventData>(Id);
template <>
PERFETTO_WARN_UNUSED_RESULT FtraceData
    TraceTokenBuffer::Extract<FtraceData>(Id);
template <>
PERFETTO_WARN_UNUSED_RESULT TracePacketData
    TraceTokenBuffer::Extract<TracePacketData>(Id);

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SORTER_TRACE_TOKEN_BUFFER_H_
