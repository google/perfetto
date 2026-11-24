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

#include "src/trace_processor/core/util/arena.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto::trace_processor::core {
namespace {

// Most plans fit in one or two blocks.
constexpr size_t kBlockBytes = 4096;

}  // namespace

Arena::~Arena() {
  FreeBlocks();
}

void Arena::FreeBlocks() {
  while (blocks_) {
    Block* previous = blocks_->prev;
    free(blocks_);
    blocks_ = previous;
  }
}

void* Arena::Allocate(size_t size, size_t align) {
  if (size == 0) {
    return nullptr;
  }
  uintptr_t at =
      static_cast<uintptr_t>(base::AlignUp(static_cast<size_t>(next_), align));
  if (PERFETTO_LIKELY(at <= end_ && size <= end_ - at)) {
    next_ = at + size;
    return reinterpret_cast<void*>(at);
  }
  return AllocateBlock(size, align);
}

void* Arena::AllocateBlock(size_t size, size_t align) {
  size_t bytes = std::max(kBlockBytes, sizeof(Block) + size + align);
  auto* block = static_cast<Block*>(malloc(bytes));
  PERFETTO_CHECK(block);
  uintptr_t block_end = reinterpret_cast<uintptr_t>(block) + bytes;
  uintptr_t at = static_cast<uintptr_t>(base::AlignUp(
      static_cast<size_t>(reinterpret_cast<uintptr_t>(block) + sizeof(Block)),
      align));
  uintptr_t block_next = at + size;
  if (blocks_ && block_end - block_next < end_ - next_) {
    // Keep using the active block when an oversized allocation leaves less
    // free space than the active block still has.
    block->prev = blocks_->prev;
    blocks_->prev = block;
  } else {
    block->prev = blocks_;
    blocks_ = block;
    next_ = block_next;
    end_ = block_end;
  }
  return reinterpret_cast<void*>(at);
}

}  // namespace perfetto::trace_processor::core
