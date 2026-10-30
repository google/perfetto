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

#include "src/trace_processor/core/exec/buffer_pool.h"

#include <cstdint>
#include <memory>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {

BufferPool::BufferPool() = default;

BufferPool::~BufferPool() {
#if PERFETTO_DCHECK_IS_ON()
  for (const auto& block : blocks_) {
    PERFETTO_DCHECK(block->references == 0);
  }
#endif
}

ColumnBuffer::Block* BufferPool::NewBlock(uint32_t width_class) {
  blocks_.push_back(std::make_unique<ColumnBuffer::Block>());
  ColumnBuffer::Block* block = blocks_.back().get();
  block->pool = this;
  block->width_class = width_class;
  uint64_t bytes = uint64_t{kMaxBatchRows} * (4u << width_class);
  block->chunk.storage = Slab<uint8_t>::Alloc(bytes);
  allocated_bytes_ += bytes;
  return block;
}

}  // namespace perfetto::trace_processor::core::exec
