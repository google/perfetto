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

#ifndef SRC_TRACING_SERVICE_BLOCK_ALLOCATOR_H_
#define SRC_TRACING_SERVICE_BLOCK_ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>

#include <list>
#include <memory>

#include "perfetto/ext/base/thread_checker.h"

namespace perfetto {
namespace internal {

// A bump allocator for short-lived data.
// Allocations share blocks, and the last one destroyed frees the block.
class BlockAllocator {
 private:
  struct Block {
    Block(BlockAllocator* block_allocator, size_t block_capacity);
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&&) = delete;
    Block& operator=(Block&&) = delete;

    BlockAllocator* const allocator;
    const std::unique_ptr<uint8_t[]> data;
    const size_t capacity;
    size_t used_size = 0;
    size_t num_live_allocations = 0;
  };
  using BlockIterator = std::list<Block>::iterator;

 public:
  // Holds the bytes from Allocate(), which stay valid until this is destroyed.
  // Destroy it before the allocator, which CHECKs that no allocations remain.
  class Allocation {
   public:
    Allocation() = default;
    ~Allocation();
    Allocation(Allocation&& other) noexcept;
    Allocation& operator=(Allocation&& other) noexcept;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;

    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }

   private:
    friend class BlockAllocator;
    Allocation(BlockIterator block, uint8_t* data, size_t size);

    BlockIterator block_{};
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
  };

  explicit BlockAllocator(size_t block_size);
  ~BlockAllocator();
  BlockAllocator(const BlockAllocator&) = delete;
  BlockAllocator& operator=(const BlockAllocator&) = delete;
  BlockAllocator(BlockAllocator&&) = delete;
  BlockAllocator& operator=(BlockAllocator&&) = delete;

  // Returns |size| uninitialized bytes, with no alignment guarantee.
  // A request larger than |block_size_| gets its own block of |size| bytes.
  Allocation Allocate(size_t size);

  // Returns the total capacity in bytes of active and free blocks.
  size_t GetMemoryUsageBytes() const;

 private:
  static constexpr size_t kMaxFreeBlocks = 2;

  // Called when the last Allocation of |block| is destroyed.
  // Moves |block| to |free_blocks_| for reuse if it has |block_size_| bytes and
  // the list holds fewer than |kMaxFreeBlocks| blocks, otherwise destroys it.
  void ReleaseBlock(BlockIterator block);

  // Capacity of a new block, unless the request is larger.
  // Only blocks of this capacity are cached for reuse.
  const size_t block_size_;

  // Blocks with allocations in use, oldest first.
  std::list<Block> active_blocks_;

  // Empty blocks of |block_size_| bytes kept for reuse.
  std::list<Block> free_blocks_;

  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_BLOCK_ALLOCATOR_H_
