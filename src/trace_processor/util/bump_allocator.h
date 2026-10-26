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

#ifndef SRC_TRACE_PROCESSOR_UTIL_BUMP_ALLOCATOR_H_
#define SRC_TRACE_PROCESSOR_UTIL_BUMP_ALLOCATOR_H_

#include <cstdint>
#include <cstring>
#include <limits>
#include <tuple>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/util/chunk_memory.h"

namespace perfetto::trace_processor {

// A simple memory allocator which "bumps" a pointer to service allocations.
// See [1] for more details for an overview of bump allocators.
//
// This implementation works by obtaining a large chunk of memory from the
// system allocator (i.e. from malloc). Every allocation uses that chunk as long
// as there is free space inside. Once an allocation is requested which does not
// fit in that chunk, a new chunk is requested from the system.
//
// IMPORTANT: all allocations returned from this allocator are 8-aligned and
// all allocation sizes must be a multiple of 8.
//
// IMPORTANT: this allocator can allocate a total of 4GB of memory (2^32). Once
// this is exhausted, any further allocation will cause a CHECK.
//
// IMPORTANT: all allocations *must* be explicitly freed before destroying this
// object. The destructor will CHECK if it detects any allocation which is
// unfreed.
//
// [1] https://rust-hosted-langs.github.io/book/chapter-simple-bump.html
class BumpAllocator {
 public:
  // The limit on the total number of bits which can be used to represent
  // the chunk id.
  static constexpr uint64_t kMaxIdBits = 58;

  // The limit on the total amount of memory which can be allocated.
  static constexpr uint64_t kAllocLimit = 1ull << kMaxIdBits;

  // The size of the "large chunk" requested from the system allocator.
  // The size of this value trades-off between unused memory use vs CPU cost
  // of going to the system allocator (and of paging chunks out, as each chunk
  // is paged in one read or write).
  static constexpr uint64_t kChunkSize = 64ull * 1024;  // 64KB

  // The maximum number of chunks which this allocator can have.
  static constexpr uint64_t kMaxChunkCount = kAllocLimit / kChunkSize;

  // The number of bits used to represent the offset the chunk in AllocId.
  //
  // This is simply log2(kChunkSize): we have a separate constant as log2 is
  // not a constexpr function: the static assets below verify this stays in
  // sync.
  static constexpr uint64_t kChunkOffsetAllocIdBits = 16u;

  // The number of bits used to represent the chunk index in AllocId.
  static constexpr uint64_t kChunkIndexAllocIdBits =
      kMaxIdBits - kChunkOffsetAllocIdBits;

  // Represents an allocation returned from the allocator. We return this
  // instead of just returning a pointer to allow looking up a chunk an
  // allocation belongs to without needing having to scan chunks.
  struct AllocId {
    uint64_t chunk_index : kChunkIndexAllocIdBits;
    uint64_t chunk_offset : kChunkOffsetAllocIdBits;

    // Comparison operators mainly for sorting.
    bool operator<(const AllocId& other) const {
      return std::tie(chunk_index, chunk_offset) <
             std::tie(other.chunk_index, other.chunk_offset);
    }
    bool operator>=(const AllocId& other) const { return !(*this < other); }
    bool operator>(const AllocId& other) const { return other < *this; }
  };
  static_assert(sizeof(AllocId) == sizeof(uint64_t),
                "AllocId should be 64-bit in size to allow serialization");
  static_assert(
      kMaxChunkCount == (1ull << kChunkIndexAllocIdBits),
      "Max chunk count must match the number of bits used for chunk indices");
  static_assert(
      kChunkSize == (1 << kChunkOffsetAllocIdBits),
      "Chunk size must match the number of bits used for offset within chunk");

  // Takes chunks from the heap.
  BumpAllocator();
  // |memory|'s chunk size must be kChunkSize.
  explicit BumpAllocator(RefPtr<ChunkMemory> memory);

  // Verifies that all calls to |Alloc| were paired with matching calls to
  // |Free|.
  ~BumpAllocator();

  BumpAllocator(BumpAllocator&&) noexcept = default;
  BumpAllocator& operator=(BumpAllocator&&) noexcept = default;

  // Allocates |size| bytes of memory. |size| must be a multiple of 8 and less
  // than or equal to |kChunkSize|.
  //
  // Returns an |AllocId| which can be converted to a pointer using
  // |GetPointer|; |*ptr| receives that pointer directly.
  AllocId Alloc(uint32_t size, void** ptr) {
    // Size is required to be a multiple of 8 to avoid needing to deal with
    // alignment. It must also be at most kChunkSize as we do not support cross
    // chunk spanning allocations.
    PERFETTO_DCHECK(size % 8 == 0);
    PERFETTO_DCHECK(size <= kChunkSize);
    if (PERFETTO_UNLIKELY(chunks_.empty() ||
                          chunks_.back().bump_offset + size > kChunkSize)) {
      AddChunk();
    }
    Chunk& chunk = chunks_.back();
    uint32_t offset = chunk.bump_offset;
    chunk.bump_offset = offset + size;
    chunk.unfreed_allocations++;
    // Unpoison the allocation range to allow access to it on ASAN builds.
    PERFETTO_ASAN_UNPOISON(chunk.data + offset, size);
    *ptr = chunk.data + offset;
    return AllocId{LastChunkIndex(), offset};
  }
  AllocId Alloc(uint32_t size) {
    void* ptr;
    return Alloc(size, &ptr);
  }

  // Frees an allocation previously allocated by |Alloc|. This function is *not*
  // idempotent.
  //
  // Once this function returns, |id| is no longer valid for any use. Trying
  // to use it further (e.g. to passing to other methods including Free itself)
  // will cause undefined behaviour.
  void Free(AllocId id);

  // Given an AllocId, returns a pointer which can be read from/written to,
  // paging its chunk back in if it was paged out.
  //
  // The caller is only allowed to access up to |size| bytes, where |size| ==
  // the |size| argument to Alloc.
  void* GetPointer(AllocId);

  // A resolved allocation for reading and then freeing with one chunk lookup.
  // Free it before allocating again or erasing chunks, which can move metadata.
  class Allocation {
   public:
    void* data() const { return data_; }
    // The blob owning the allocation's chunk. Views of it keep the chunk's
    // memory alive after the allocator drops the chunk.
    const RefPtr<TraceBlob>& chunk_blob() const { return *chunk_blob_; }

   private:
    friend class BumpAllocator;
    Allocation(void* data,
               uint32_t* unfreed_allocations,
               const RefPtr<TraceBlob>* chunk_blob)
        : data_(data),
          unfreed_allocations_(unfreed_allocations),
          chunk_blob_(chunk_blob) {}

    void* data_;
    uint32_t* unfreed_allocations_;
    const RefPtr<TraceBlob>* chunk_blob_;
  };

  // Like GetPointer, the allocation's chunk is paged in if needed.
  Allocation GetAllocation(AllocId);

  // Keeps the chunk of the last allocation resident: it holds an object which
  // cannot be moved by copying its bytes.
  void PinLastChunk() { chunks_.back().pinned = true; }

  // Returns true if this freed the last allocation in its chunk.
  bool Free(const Allocation& allocation) {
    PERFETTO_DCHECK(*allocation.unfreed_allocations_ > 0);
    return --*allocation.unfreed_allocations_ == 0;
  }

  // Frees every allocation, keeping the last chunk for reuse.
  void Clear();

  // Removes chunks from the start of this allocator where all the allocations
  // in the chunks have been freed. This releases the memory back to the system.
  //
  // Returns the number of chunks freed.
  uint64_t EraseFrontFreeChunks();

  // Returns a "past the end" serialized AllocId i.e. a serialized value
  // greater than all previously returned AllocIds.
  AllocId PastTheEndId();

  // Gives back the memory of the chunk |chunk_index|, which must not be
  // erased, if all its allocations are freed and it is not the one being
  // allocated from. Its metadata stays until EraseFrontFreeChunks reaches it,
  // so a drained chunk behind an undrained one costs no memory.
  void ReleaseIfDrained(uint64_t chunk_index);

  // Whether the chunk |chunk_index|, which must not be erased, still has
  // unfreed allocations.
  bool HasUnfreed(uint64_t chunk_index) {
    return chunks_.at(static_cast<size_t>(ChunkIndexToQueueIndex(chunk_index)))
               .unfreed_allocations > 0;
  }

  // Returns the number of erased chunks from the start of this allocator.
  //
  // This value may change any time |EraseFrontFreeChunks| is called but is
  // constant otherwise.
  uint64_t erased_front_chunks_count() const {
    return erased_front_chunks_count_;
  }

 private:
  struct Chunk {
    // The chunk's memory. It is a TraceBlob so views of what is stored in it
    // can outlive the chunk in this allocator. Heap blobs are at least 16-byte
    // aligned, which covers the 8-byte alignment all allocations need.
    RefPtr<TraceBlob> blob;
    uint8_t* data = nullptr;

    // The bump offset relative to |data|. Incremented to service
    // Alloc requests.
    uint32_t bump_offset = 0;

    // The number of unfreed allocations in this chunk.
    uint32_t unfreed_allocations = 0;

    // While paged out, |blob| and |data| are null and |page_slot| holds the
    // contents.
    uint32_t page_slot = 0;

    // Whether the chunk must never be paged out.
    bool pinned = false;

    // Whether the chunk drained and gave its memory back: |blob| and |data|
    // are null and there is no page slot.
    bool released = false;
  };

  // Appends a chunk for the allocations the last one cannot fit.
  void AddChunk();

  // When over the resident budget, pages out the last chunk, which has just
  // filled up: the sorter reads chunks back roughly in the order they were
  // written, so it is the one needed last. A pinned chunk, or one with views of
  // its memory alive, stays resident.
  void MaybePageOutLast();
  void PageIn(Chunk&);
  // Frees the memory or page slot |chunk| holds, if any.
  void Drop(Chunk&);

  Chunk& ResidentChunk(uint64_t chunk_index) {
    uint64_t queue_index = ChunkIndexToQueueIndex(chunk_index);
    PERFETTO_CHECK(queue_index <= std::numeric_limits<size_t>::max());
    Chunk& chunk = chunks_.at(static_cast<size_t>(queue_index));
    PERFETTO_DCHECK(!chunk.released);
    if (PERFETTO_UNLIKELY(!chunk.data)) {
      PageIn(chunk);
    }
    return chunk;
  }

  uint64_t ChunkIndexToQueueIndex(uint64_t chunk_index) const {
    return chunk_index - erased_front_chunks_count_;
  }
  uint64_t QueueIndexToChunkIndex(uint64_t index_in_chunks_vec) const {
    return erased_front_chunks_count_ + index_in_chunks_vec;
  }
  uint64_t LastChunkIndex() const {
    PERFETTO_DCHECK(!chunks_.empty());
    return QueueIndexToChunkIndex(static_cast<uint64_t>(chunks_.size() - 1));
  }

  RefPtr<ChunkMemory> memory_;
  base::CircularQueue<Chunk> chunks_;
  uint64_t erased_front_chunks_count_ = 0;
  size_t resident_chunks_ = 0;
  size_t budget_chunks_ = 0;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_BUMP_ALLOCATOR_H_
