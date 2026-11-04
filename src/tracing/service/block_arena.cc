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
#include <memory>
#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace internal {

struct BlockArena::Block {
  explicit Block(size_t block_capacity)
      : data(new uint8_t[block_capacity]), capacity(block_capacity) {
    PERFETTO_ASAN_POISON(data.get(), capacity);
  }

  const std::unique_ptr<uint8_t[]> data;
  const size_t capacity;
  size_t used_size = 0;
  size_t num_live_allocations = 0;
};

BlockArena::BlockArena(size_t block_size) : block_size_(block_size) {
  PERFETTO_DCHECK(block_size > 0);
}

BlockArena::~BlockArena() {
  // Allocations call back into the arena, so they must all be destroyed first.
  PERFETTO_CHECK(active_blocks_.empty());
}

BlockArena::Allocation BlockArena::Allocate(size_t size) {
  PERFETTO_DCHECK(size > 0);

  // Only the last active block takes new allocations.
  Block* block = active_blocks_.empty() ? nullptr : &active_blocks_.back();
  if (!block || size > block->capacity - block->used_size) {
    if (!free_blocks_.empty() && size <= free_blocks_.back().capacity) {
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
  return Allocation(this, block, data_begin, size);
}

void BlockArena::ReleaseBlock(Block* block) {
  auto it =
      std::find_if(active_blocks_.begin(), active_blocks_.end(),
                   [block](const Block& active) { return &active == block; });
  PERFETTO_DCHECK(it != active_blocks_.end());

  if (block->capacity == block_size_ && free_blocks_.size() < kMaxFreeBlocks) {
    block->used_size = 0;
    PERFETTO_ASAN_POISON(block->data.get(), block->capacity);
    free_blocks_.splice(free_blocks_.end(), active_blocks_, it);
  } else {
    active_blocks_.erase(it);
  }
}

size_t BlockArena::GetMemoryUsageBytes() const {
  size_t bytes = 0;
  for (const Block& block : active_blocks_)
    bytes += block.capacity;
  for (const Block& block : free_blocks_)
    bytes += block.capacity;
  return bytes;
}

BlockArena::Allocation::Allocation(BlockArena* arena,
                                   Block* block,
                                   uint8_t* data,
                                   size_t size)
    : arena_(arena), block_(block), data_(data), size_(size) {
  ++block_->num_live_allocations;
}

BlockArena::Allocation::~Allocation() {
  if (!block_)
    return;
  PERFETTO_CHECK(block_->num_live_allocations > 0);
  PERFETTO_ASAN_POISON(data_, size_);
  --block_->num_live_allocations;
  if (block_->num_live_allocations == 0)
    arena_->ReleaseBlock(block_);
}

BlockArena::Allocation::Allocation(Allocation&& other) noexcept
    : arena_(std::exchange(other.arena_, nullptr)),
      block_(std::exchange(other.block_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0u)) {}

BlockArena::Allocation& BlockArena::Allocation::operator=(
    Allocation&& other) noexcept {
  this->~Allocation();                      // Destroy the current state.
  new (this) Allocation(std::move(other));  // Use the move ctor above.
  return *this;
}

}  // namespace internal
}  // namespace perfetto
