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

#include "src/tracing/service/decode_arena.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <algorithm>
#include <new>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace internal {

DecodeArena::DecodeArena(size_t block_size) : block_size_(block_size) {
  PERFETTO_DCHECK(block_size > 0);
}

DecodeArena::~DecodeArena() {
  DeleteBlocks(&active_blocks_);
  DeleteBlocks(&free_blocks_);
}

DecodeArena::Allocation DecodeArena::Allocate(size_t size) {
  PERFETTO_DCHECK(size > 0);
  // Use only the front block to avoid scanning older blocks for space.
  // This can leave unused tails in older blocks.
  Block* block = active_blocks_.empty() ? nullptr : &active_blocks_.front();
  if (!block || size > block->capacity - block->used_size) {
    if (size <= block_size_ && !free_blocks_.empty()) {
      block = &free_blocks_.front();
      free_blocks_.PopFront();
    } else {
      block = new Block(std::max(block_size_, size));
    }
    active_blocks_.PushFront(*block);
  }

  uint8_t* data_begin = block->data + block->used_size;
  // Reset() poisons cached blocks, so allow access to the bytes we return.
  PERFETTO_ASAN_UNPOISON(data_begin, size);
  block->used_size += size;
  return Allocation(block, data_begin, size);
}

void DecodeArena::Reset() {
  // Count live ordinary blocks first because they must survive Reset() and
  // consume the retention budget before we cache any other blocks.
  size_t num_retained_ordinary_blocks = free_blocks_.size();
  for (const Block& block : active_blocks_) {
    if (block.capacity == block_size_ && block.num_live_allocations > 0)
      ++num_retained_ordinary_blocks;
  }

  for (auto it = active_blocks_.begin(); it != active_blocks_.end();) {
    Block& block = *it;
    // Advance before this block is deleted or moved to free_blocks_.
    ++it;

    if (block.num_live_allocations != 0)
      continue;

    active_blocks_.Erase(block);
    if (block.capacity == block_size_ &&
        num_retained_ordinary_blocks < kMaxRetainedBlocks) {
      block.used_size = 0;
      // Poison reclaimed bytes to help detect stale packet slices until reuse.
      PERFETTO_ASAN_POISON(block.data, block.capacity);
      free_blocks_.PushFront(block);
      ++num_retained_ordinary_blocks;
    } else {
      delete &block;
    }
  }
}

void DecodeArena::Compact() {
  Reset();
  DeleteBlocks(&free_blocks_);
}

size_t DecodeArena::GetMemoryUsageBytes() const {
  size_t bytes = free_blocks_.size() * block_size_;
  for (const Block& block : active_blocks_)
    bytes += block.capacity;
  return bytes;
}

DecodeArena::Allocation::Allocation(Block* block, uint8_t* data, size_t size)
    : block_(block), data_(data), size_(size) {
  ++block_->num_live_allocations;
}

DecodeArena::Allocation::~Allocation() {
  Release();
}

DecodeArena::Allocation::Allocation(Allocation&& other) noexcept
    : block_(std::exchange(other.block_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0u)) {}

DecodeArena::Allocation& DecodeArena::Allocation::operator=(
    Allocation&& other) noexcept {
  this->~Allocation();
  new (this) Allocation(std::move(other));
  return *this;
}

void DecodeArena::Allocation::Release() {
  if (!block_)
    return;
  PERFETTO_CHECK(block_->num_live_allocations > 0);
  // Keep the bytes intact because this batch can still hold packet slices.
  --block_->num_live_allocations;
  block_ = nullptr;
  data_ = nullptr;
  size_ = 0;
}

DecodeArena::Block::Block(size_t block_capacity)
    : data(new uint8_t[block_capacity]), capacity(block_capacity) {}

DecodeArena::Block::~Block() {
  // A live Allocation would later access this block when it's released.
  PERFETTO_CHECK(num_live_allocations == 0);
  delete[] data;
}

// static
void DecodeArena::DeleteBlocks(BlockList* blocks) {
  while (!blocks->empty()) {
    Block& block = blocks->front();
    blocks->PopFront();
    delete &block;
  }
}

}  // namespace internal
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
