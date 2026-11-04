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

#include "src/tracing/service/block_arena.h"

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace internal {

BlockArena::BlockArena(size_t block_size) : block_size_(block_size) {
  PERFETTO_DCHECK(block_size > 0);
}

BlockArena::Allocation BlockArena::Allocate(size_t size) {
  PERFETTO_DCHECK(size > 0);

  // We don't use any older active blocks to keep the logic simple.
  Block* block = active_blocks_.empty() ? nullptr : &active_blocks_.back();
  if (!block || size > block->capacity - block->used_size) {
    if (size <= block_size_ && !free_blocks_.empty()) {
      active_blocks_.splice(active_blocks_.end(), free_blocks_,
                            std::prev(free_blocks_.end()));
    } else {
      active_blocks_.emplace_back(std::max(block_size_, size));
    }
    block = &active_blocks_.back();
  }

  uint8_t* data_begin = block->data.get() + block->used_size;
  PERFETTO_ASAN_UNPOISON(data_begin, size);
  block->used_size += size;
  return Allocation(block, data_begin, size);
}

void BlockArena::Reset() {
  for (auto next = active_blocks_.begin(); next != active_blocks_.end();) {
    // Advance before this block is deleted or moved to |free_blocks_|.
    auto block = next++;

    if (block->num_live_allocations != 0)
      continue;

    if (block->capacity == block_size_ &&
        free_blocks_.size() < kMaxFreeBlocks) {
      block->used_size = 0;
      PERFETTO_ASAN_POISON(block->data.get(), block->capacity);
      free_blocks_.splice(free_blocks_.end(), active_blocks_, block);
    } else {
      active_blocks_.erase(block);
    }
  }
}

size_t BlockArena::GetMemoryUsageBytes() const {
  size_t bytes = free_blocks_.size() * block_size_;
  for (const Block& block : active_blocks_)
    bytes += block.capacity;
  return bytes;
}

BlockArena::Allocation::Allocation(Block* block, uint8_t* data, size_t size)
    : block_(block), data_(data), size_(size) {
  ++block_->num_live_allocations;
}

BlockArena::Allocation::~Allocation() {
  if (!block_)
    return;
  PERFETTO_CHECK(block_->num_live_allocations > 0);
  --block_->num_live_allocations;
}

BlockArena::Allocation::Allocation(Allocation&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0u)) {}

BlockArena::Allocation& BlockArena::Allocation::operator=(
    Allocation&& other) noexcept {
  this->~Allocation();                      // Destroy the current state.
  new (this) Allocation(std::move(other));  // Use the move ctor above.
  return *this;
}

BlockArena::Block::Block(size_t block_capacity)
    : data(new uint8_t[block_capacity]), capacity(block_capacity) {
  PERFETTO_ASAN_POISON(data.get(), capacity);
}

BlockArena::Block::~Block() {
  // A live Allocation would access this block when it's destroyed.
  PERFETTO_CHECK(num_live_allocations == 0);
}

}  // namespace internal
}  // namespace perfetto
