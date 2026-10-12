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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/exec/column_chunk.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// The storage one column of a batch is in, if the batch owns it rather than
// borrowing storage which outlives the run: up to kMaxBatchRows values and
// which of them are there.
//
// A handle: copies share the storage, and the last to go gives it back to the
// Context it came from. It is only ever used from one thread, so counting
// copies costs no atomics. Whoever takes it from the Context fills it before
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
  friend class Context;

  struct Block {
    ColumnChunk chunk;
    Context* context = nullptr;
    uint32_t references = 0;
    // While no handle holds it, the next block the Context can give out.
    Block* next_free = nullptr;
  };

  explicit ColumnBuffer(Block* block) : block_(block) { ++block_->references; }
  inline void Release();

  Block* block_ = nullptr;
};

// What one execution of a plan shares as it runs: the storage the batches'
// own columns are in. A step takes a buffer for each column it fills and
// hands it to the batch; once no batch holds it, the buffer comes back to be
// taken again, the last given back first, while it is likely still in cache.
//
// Must outlive every batch holding one of its buffers.
class Context {
 public:
  Context();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  // A buffer no batch holds, holding whatever it last held.
  ColumnBuffer TakeBuffer() {
    ColumnBuffer::Block* block = free_;
    if (PERFETTO_UNLIKELY(!block)) {
      return ColumnBuffer(NewBlock());
    }
    free_ = block->next_free;
    return ColumnBuffer(block);
  }

 private:
  friend class ColumnBuffer;

  ColumnBuffer::Block* NewBlock();
  void GiveBack(ColumnBuffer::Block* block) {
    block->next_free = free_;
    free_ = block;
  }

  std::vector<std::unique_ptr<ColumnBuffer::Block>> blocks_;
  ColumnBuffer::Block* free_ = nullptr;
};

inline void ColumnBuffer::Release() {
  if (block_ && --block_->references == 0) {
    block_->context->GiveBack(block_);
  }
  block_ = nullptr;
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONTEXT_H_
