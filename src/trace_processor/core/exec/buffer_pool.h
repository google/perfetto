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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_chunk.h"

namespace perfetto::trace_processor::core::exec {

class BufferPool;

// The storage one column of a batch is in, if the batch owns it rather than
// borrowing storage which outlives the run: up to kMaxBatchRows values and
// which of them are there.
//
// A handle: copies share the storage, and the last to go gives it back to the
// BufferPool it came from. It is only ever used from one thread, so counting
// copies costs no atomics. Whoever takes it from the pool fills it before
// sharing it; nothing writes to it after.
class ColumnBuffer {
 public:
  ColumnBuffer() = default;
  ColumnBuffer(const ColumnBuffer& other) : block_(other.block_) {
    if (block_) {
      ++block_->references;
    }
  }
  ColumnBuffer& operator=(const ColumnBuffer& other) {
    ColumnBuffer copy(other);
    std::swap(block_, copy.block_);
    return *this;
  }
  ColumnBuffer(ColumnBuffer&& other) noexcept
      : block_(std::exchange(other.block_, nullptr)) {}
  ColumnBuffer& operator=(ColumnBuffer&& other) noexcept {
    if (this != &other) {
      Release();
      block_ = std::exchange(other.block_, nullptr);
    }
    return *this;
  }
  ~ColumnBuffer() { Release(); }

  explicit operator bool() const { return block_ != nullptr; }

  ColumnChunk& chunk() const { return block_->chunk; }

 private:
  friend class BufferPool;

  struct Block {
    ColumnChunk chunk;
    BufferPool* pool = nullptr;
    uint32_t references = 0;
    // Which of the pool's widths the chunk's storage is sized for.
    uint32_t width_class = 0;
    // While no handle holds it, the next block of its width the pool can give
    // out.
    Block* next_free = nullptr;
  };

  explicit ColumnBuffer(Block* block) : block_(block) { ++block_->references; }
  inline void Release();

  Block* block_ = nullptr;
};

// The buffers the batches of one execution own their columns in, kept by the
// width of the values they hold: 4, 8 or 16 bytes (see ValueWidth). Each
// buffer's storage is sized for its width, so a column takes no more memory
// than its values need.
//
// Once no batch holds a buffer, it comes back to the free list of its width
// and is the next of that width given out, while it is likely still in cache.
// Buffers are kept until the pool goes, so a run which has reached its working
// set allocates nothing more.
class BufferPool {
 public:
  BufferPool();
  ~BufferPool();
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // A buffer no batch holds, for values `width` bytes wide, holding whatever
  // it last held.
  PERFETTO_ALWAYS_INLINE ColumnBuffer Take(uint32_t width) {
    uint32_t width_class = ClassOf(width);
    ColumnBuffer::Block* block = free_[width_class];
    if (PERFETTO_UNLIKELY(!block)) {
      return ColumnBuffer(NewBlock(width_class));
    }
    free_[width_class] = block->next_free;
    return ColumnBuffer(block);
  }

  // The bytes of value storage the pool has allocated, held or free.
  uint64_t allocated_bytes() const { return allocated_bytes_; }

 private:
  friend class ColumnBuffer;

  // One free list per width: 4, 8 and 16 bytes.
  static constexpr uint32_t kWidthClasses = 3;
  static_assert(ColumnChunk::kMaxValueSize == 4u << (kWidthClasses - 1));

  static PERFETTO_ALWAYS_INLINE uint32_t ClassOf(uint32_t width) {
    PERFETTO_DCHECK(width == 4 || width == 8 || width == 16);
    return width == 4 ? 0 : (width == 8 ? 1 : 2);
  }

  ColumnBuffer::Block* NewBlock(uint32_t width_class);
  void GiveBack(ColumnBuffer::Block* block) {
    block->next_free = free_[block->width_class];
    free_[block->width_class] = block;
  }

  std::vector<std::unique_ptr<ColumnBuffer::Block>> blocks_;
  std::array<ColumnBuffer::Block*, kWidthClasses> free_{};
  uint64_t allocated_bytes_ = 0;
};

inline void ColumnBuffer::Release() {
  if (block_ && --block_->references == 0) {
    block_->pool->GiveBack(block_);
  }
  block_ = nullptr;
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_
