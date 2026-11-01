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

#ifndef SRC_TRACING_SERVICE_DECODE_ARENA_H_
#define SRC_TRACING_SERVICE_DECODE_ARENA_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/ext/base/intrusive_list.h"

namespace perfetto {
namespace internal {

// Holds decompressed packet data in reusable blocks.
// Each Allocation keeps its block in use across Reset() and Compact().
// Releasing an Allocation leaves its bytes intact until the next such call,
// because returned packet slices can still refer to them.
// Serialize access and destroy all Allocations before the arena.
class DecodeArena {
  struct Block;

 public:
  // Keeps its block in use until it's released.
  // Move-only, and a moved-from Allocation is empty.
  class Allocation {
   public:
    Allocation() = default;
    ~Allocation();
    Allocation(Allocation&& other) noexcept;
    Allocation& operator=(Allocation&& other) noexcept;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;

    // Ends the hold on the block and makes this Allocation empty.
    // The bytes stay readable until the next Reset() or Compact().
    void Release();

    uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return block_ != nullptr; }

   private:
    friend class DecodeArena;
    Allocation(Block* block, uint8_t* data, size_t size);

    Block* block_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
  };

  explicit DecodeArena(size_t block_size);
  ~DecodeArena();
  DecodeArena(const DecodeArena&) = delete;
  DecodeArena& operator=(const DecodeArena&) = delete;
  DecodeArena(DecodeArena&&) = delete;
  DecodeArena& operator=(DecodeArena&&) = delete;

  // Returns |size| uninitialized bytes, with |size| > 0.
  // Bytes that don't fit in an ordinary block of |block_size_| bytes get an
  // oversized block of exactly |size| bytes.
  Allocation Allocate(size_t size);

  // Call once borrowed slices from the previous batch are no longer used.
  // Preserves live Allocations and reclaims blocks with no live Allocations.
  // Caches unused ordinary blocks within the retention target below.
  void Reset();

  // Like Reset(), but releases all spare blocks instead of caching them.
  void Compact();

  // Returns the total capacity in bytes of active and cached blocks.
  size_t GetMemoryUsageBytes() const;

 private:
  struct Block {
    explicit Block(size_t block_capacity);
    ~Block();
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    Block(Block&&) = delete;
    Block& operator=(Block&&) = delete;

    struct ListTraits {
      static constexpr size_t node_offset() { return offsetof(Block, node); }
    };

    // ListTraits uses offsetof(), so Block must stay standard-layout.
    // With clang and libstdc++, a unique_ptr member breaks that requirement.
    // ~Block() deletes |data|.
    uint8_t* data;
    const size_t capacity;
    size_t used_size = 0;
    // Allocation owners, excluding borrowed packet slices.
    size_t num_live_allocations = 0;

    base::IntrusiveListNode node{};
  };
  using BlockList = base::IntrusiveList<Block, Block::ListTraits>;

  static void DeleteBlocks(BlockList* blocks);

  // Target for ordinary blocks retained between batches, including live blocks.
  // Keeping two allows reuse for batches that need two blocks.
  // Live blocks can exceed this target because their Allocations must survive.
  static constexpr size_t kMaxRetainedBlocks = 2;

  // Capacity of each reusable block.
  const size_t block_size_;

  // Owns blocks used in this batch and older blocks with live Allocations.
  BlockList active_blocks_;

  // Empty ordinary blocks kept for reuse.
  BlockList free_blocks_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_DECODE_ARENA_H_
