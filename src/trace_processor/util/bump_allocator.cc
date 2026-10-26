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

#include "src/trace_processor/util/bump_allocator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto::trace_processor {
namespace {

RefPtr<TraceBlob> Allocate(const ChunkMemory* memory) {
  RefPtr<TraceBlob> blob(new TraceBlob(memory->AllocateChunk()));
  // Poison the region to try and catch out of bound accesses.
  PERFETTO_ASAN_POISON(blob->mutable_data(), blob->size());
  return blob;
}

}  // namespace

BumpAllocator::BumpAllocator() : BumpAllocator(ChunkMemory::Heap(kChunkSize)) {}

BumpAllocator::BumpAllocator(RefPtr<ChunkMemory> memory)
    : memory_(std::move(memory)) {
  PERFETTO_CHECK(memory_->chunk_size() == kChunkSize);
  size_t budget = memory_->resident_budget();
  budget_chunks_ = budget == ChunkMemory::kNoBudget
                       ? std::numeric_limits<size_t>::max()
                       : std::max<size_t>(budget / kChunkSize, 2);
}

BumpAllocator::~BumpAllocator() {
  for (auto& chunk : chunks_) {
    PERFETTO_CHECK(chunk.unfreed_allocations == 0);
    Drop(chunk);
  }
}

void BumpAllocator::AddChunk() {
  if (resident_chunks_ >= budget_chunks_) {
    MaybePageOutLast();
  }
  Chunk chunk;
  chunk.blob = Allocate(memory_.get());
  chunk.data = chunk.blob->mutable_data();
  chunks_.emplace_back(std::move(chunk));
  resident_chunks_++;

  // Ensure that we haven't exceeded the maximum number of chunks.
  PERFETTO_CHECK(LastChunkIndex() < kMaxChunkCount);
}

void BumpAllocator::Free(AllocId id) {
  uint64_t queue_index = ChunkIndexToQueueIndex(id.chunk_index);
  PERFETTO_DCHECK(queue_index <= std::numeric_limits<size_t>::max());
  Chunk& chunk = chunks_.at(static_cast<size_t>(queue_index));
  PERFETTO_DCHECK(chunk.unfreed_allocations > 0);
  chunk.unfreed_allocations--;
}

void* BumpAllocator::GetPointer(AllocId id) {
  return ResidentChunk(id.chunk_index).data + id.chunk_offset;
}

BumpAllocator::Allocation BumpAllocator::GetAllocation(AllocId id) {
  Chunk& chunk = ResidentChunk(id.chunk_index);
  return Allocation(chunk.data + id.chunk_offset, &chunk.unfreed_allocations,
                    &chunk.blob);
}

void BumpAllocator::MaybePageOutLast() {
  if (chunks_.empty()) {
    return;
  }
  Chunk& chunk = chunks_.back();
  if (!chunk.data || chunk.pinned || chunk.unfreed_allocations == 0 ||
      !chunk.blob->HasOneRef()) {
    return;
  }
  // Only the allocated prefix is written: the rest is poisoned on ASAN builds.
  chunk.page_slot = memory_->PageOut(chunk.data, chunk.bump_offset);
  chunk.blob.reset();
  chunk.data = nullptr;
  resident_chunks_--;
}

void BumpAllocator::PageIn(Chunk& chunk) {
  PERFETTO_DCHECK(!chunk.released);
  chunk.blob = Allocate(memory_.get());
  chunk.data = chunk.blob->mutable_data();
  PERFETTO_ASAN_UNPOISON(chunk.data, chunk.bump_offset);
  memory_->PageIn(chunk.page_slot, chunk.data, chunk.bump_offset);
  resident_chunks_++;
}

void BumpAllocator::ReleaseIfDrained(uint64_t chunk_index) {
  uint64_t queue_index = ChunkIndexToQueueIndex(chunk_index);
  if (queue_index + 1 >= chunks_.size()) {
    // The last chunk is still allocated from.
    return;
  }
  Chunk& chunk = chunks_.at(static_cast<size_t>(queue_index));
  if (chunk.unfreed_allocations > 0 || chunk.released) {
    return;
  }
  Drop(chunk);
  chunk.released = true;
}

void BumpAllocator::Drop(Chunk& chunk) {
  if (chunk.data) {
    resident_chunks_--;
  } else if (!chunk.released) {
    memory_->FreePage(chunk.page_slot);
  }
  chunk.blob.reset();
  chunk.data = nullptr;
}

void BumpAllocator::Clear() {
  if (chunks_.empty()) {
    return;
  }
  size_t dropped = chunks_.size() - 1;
  for (size_t i = 0; i < dropped; ++i) {
    Drop(chunks_.at(i));
  }
  chunks_.erase_front(dropped);
  // The kept chunk takes the next index, so ids keep growing.
  erased_front_chunks_count_ += dropped + 1;
  Chunk& chunk = chunks_.front();
  chunk.bump_offset = 0;
  chunk.unfreed_allocations = 0;
  chunk.pinned = false;
  // Views of the old contents may still be alive: only reuse the memory if
  // nothing else holds it.
  if (!chunk.data || !chunk.blob->HasOneRef()) {
    Drop(chunk);
    chunk.blob = Allocate(memory_.get());
    chunk.data = chunk.blob->mutable_data();
    resident_chunks_++;
  }
  PERFETTO_ASAN_POISON(chunk.data, kChunkSize);
}

uint64_t BumpAllocator::EraseFrontFreeChunks() {
  size_t to_erase_chunks = 0;
  for (; to_erase_chunks < chunks_.size(); ++to_erase_chunks) {
    // Break on the first chunk which still has unfreed allocations.
    if (chunks_.at(to_erase_chunks).unfreed_allocations > 0) {
      break;
    }
  }
  for (size_t i = 0; i < to_erase_chunks; ++i) {
    Drop(chunks_.at(i));
  }
  chunks_.erase_front(to_erase_chunks);
  erased_front_chunks_count_ += to_erase_chunks;
  return to_erase_chunks;
}

BumpAllocator::AllocId BumpAllocator::PastTheEndId() {
  if (chunks_.empty()) {
    return AllocId{erased_front_chunks_count_, 0};
  }
  if (chunks_.back().bump_offset == kChunkSize) {
    return AllocId{LastChunkIndex() + 1, 0};
  }
  return AllocId{LastChunkIndex(), chunks_.back().bump_offset};
}

}  // namespace perfetto::trace_processor
