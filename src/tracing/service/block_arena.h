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

#ifndef SRC_TRACING_SERVICE_BLOCK_ARENA_H_
#define SRC_TRACING_SERVICE_BLOCK_ARENA_H_

#include <stddef.h>
#include <stdint.h>

#include <list>

namespace perfetto {
namespace internal {

// A bump allocator for short-lived data.
// Allocations share blocks, and the last one destroyed gives the block back.
class BlockArena {
  struct Block;

 public:
  // Holds the bytes from Allocate(), which stay valid until this is destroyed.
  // Destroy it before the arena, which CHECKs that no allocations remain.
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
    explicit operator bool() const { return block_ != nullptr; }

   private:
    friend class BlockArena;
    Allocation(BlockArena* arena, Block* block, uint8_t* data, size_t size);

    BlockArena* arena_ = nullptr;
    Block* block_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
  };

  explicit BlockArena(size_t block_size);
  ~BlockArena();
  BlockArena(const BlockArena&) = delete;
  BlockArena& operator=(const BlockArena&) = delete;
  BlockArena(BlockArena&&) = delete;
  BlockArena& operator=(BlockArena&&) = delete;

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
  void ReleaseBlock(Block* block);

  // Capacity of a new block, unless the request is larger.
  // Only blocks of this capacity are cached for reuse.
  const size_t block_size_;

  // Blocks with allocations in use, oldest first.
  std::list<Block> active_blocks_;

  // Empty blocks of |block_size_| bytes kept for reuse.
  std::list<Block> free_blocks_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_BLOCK_ARENA_H_
