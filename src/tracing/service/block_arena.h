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

#include <memory>
#include <vector>

namespace perfetto {
namespace internal {

// A bump allocator for data that lives for one cycle.
// Reset() starts the next cycle and reuses the blocks.
class BlockArena {
  struct Block;

 public:
  // Keeps its block alive across Reset().
  // The arena owns the block, so destroy the Allocation before the arena.
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
    Allocation(Block* block, uint8_t* data, size_t size);

    Block* block_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
  };

  explicit BlockArena(size_t block_size);
  ~BlockArena() = default;
  BlockArena(const BlockArena&) = delete;
  BlockArena& operator=(const BlockArena&) = delete;
  BlockArena(BlockArena&&) = delete;
  BlockArena& operator=(BlockArena&&) = delete;

  // Returns |size| uninitialized bytes.
  // A request larger than |block_size_| gets its own block of |size| bytes.
  Allocation Allocate(size_t size);

  // Starts a new cycle.
  // Bytes of destroyed Allocations stay valid until this call, so callers can
  // finish using pointers into them first.
  // 1. Keeps blocks with allocations in use.
  // 2. Keeps up to |kMaxFreeBlocks| unused blocks of |block_size_| bytes to
  //    avoid reallocations.
  // 3. Frees all other unused (incl. all oversized) blocks.
  void Reset();

  // Returns the total capacity in bytes of active and free blocks.
  size_t GetMemoryUsageBytes() const;

 private:
  struct Block {
    explicit Block(size_t block_capacity);
    ~Block();
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&&) = delete;
    Block& operator=(Block&&) = delete;

    std::unique_ptr<uint8_t[]> data;
    const size_t capacity;
    size_t used_size = 0;
    size_t num_live_allocations = 0;
  };

  // Allocation needs a stable Block*, so the lists hold unique_ptrs and the
  // Blocks never move.
  using BlockList = std::vector<std::unique_ptr<Block>>;

  // Keep up to two free blocks of |block_size_| bytes for reuse across cycles.
  static constexpr size_t kMaxFreeBlocks = 2;

  // Capacity of blocks that can be cached for reuse.
  const size_t block_size_;

  // Blocks used in this cycle and older blocks with allocations in use.
  BlockList active_blocks_;

  // Empty blocks of |block_size_| bytes kept for reuse.
  BlockList free_blocks_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_BLOCK_ARENA_H_
