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

#include "src/tracing/service/block_allocator.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace internal {

BlockAllocator::Block::Block(BlockAllocator* block_allocator,
                             size_t block_capacity)
    : allocator(block_allocator),
      data(new uint8_t[block_capacity]),
      capacity(block_capacity) {
  PERFETTO_ASAN_POISON(data.get(), capacity);
}

BlockAllocator::BlockAllocator(size_t block_size) : block_size_(block_size) {
  PERFETTO_DCHECK(block_size > 0);
}

BlockAllocator::~BlockAllocator() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Allocations call back into the allocator, so they must be destroyed first.
  PERFETTO_CHECK(active_blocks_.empty());
}

BlockAllocator::Allocation BlockAllocator::Allocate(size_t size) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(size > 0);

  // Only the last active block takes new allocations.
  Block* last = active_blocks_.empty() ? nullptr : &active_blocks_.back();
  if (!last || size > last->capacity - last->used_size) {
    if (!free_blocks_.empty() && size <= free_blocks_.back().capacity) {
      active_blocks_.splice(active_blocks_.end(), free_blocks_,
                            std::prev(free_blocks_.end()));
    } else {
      active_blocks_.emplace_back(this, std::max(block_size_, size));
    }
  }

  BlockIterator block = std::prev(active_blocks_.end());
  PERFETTO_CHECK(size <= block->capacity - block->used_size);
  uint8_t* data_begin = block->data.get() + block->used_size;
  PERFETTO_ASAN_UNPOISON(data_begin, size);
  block->used_size += size;
  return Allocation(block, data_begin, size);
}

void BlockAllocator::ReleaseBlock(BlockIterator block) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(block->num_live_allocations == 0);

  if (block->capacity == block_size_ && free_blocks_.size() < kMaxFreeBlocks) {
    block->used_size = 0;
    PERFETTO_ASAN_POISON(block->data.get(), block->capacity);
    free_blocks_.splice(free_blocks_.end(), active_blocks_, block);
  } else {
    active_blocks_.erase(block);
  }
}

size_t BlockAllocator::GetMemoryUsageBytes() const {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  size_t bytes = 0;
  for (const Block& block : active_blocks_)
    bytes += block.capacity;
  for (const Block& block : free_blocks_)
    bytes += block.capacity;
  return bytes;
}

BlockAllocator::Allocation::Allocation(BlockIterator block,
                                       uint8_t* data,
                                       size_t size)
    : block_(block), data_(data), size_(size) {
  ++block_->num_live_allocations;
}

BlockAllocator::Allocation::~Allocation() {
  if (!data_)
    return;
  PERFETTO_DCHECK_THREAD(block_->allocator->thread_checker_);
  PERFETTO_CHECK(block_->num_live_allocations > 0);
  PERFETTO_ASAN_POISON(data_, size_);
  --block_->num_live_allocations;
  if (block_->num_live_allocations == 0)
    block_->allocator->ReleaseBlock(block_);
}

BlockAllocator::Allocation::Allocation(Allocation&& other) noexcept
    : block_(std::exchange(other.block_, {})),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0u)) {}

BlockAllocator::Allocation& BlockAllocator::Allocation::operator=(
    Allocation&& other) noexcept {
  if (this != &other) {
    this->~Allocation();                      // Destroy the current state.
    new (this) Allocation(std::move(other));  // Use the move ctor above.
  }
  return *this;
}

}  // namespace internal
}  // namespace perfetto
