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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_BUFFER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_BUFFER_H_

#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Combines small batches column by column. Compatible backing storage is kept
// as an indexed view. Only columns spanning different backing buffers are
// packed. Published values and selections are immutable and retain their
// owners. Packing preserves duplicates, nulls and floating-point bits. String
// IDs refer to the process-lifetime pool; string payloads are not copied.
class BatchBuffer {
 public:
  uint32_t size() const { return batch_.size(); }
  base::Status Append(const RowBatch&);
  void Take(RowBatch& out) {
    out.SwapContents(batch_);
    Clear();
  }
  void Clear() {
    batch_.Reset();
    packed_.clear();
    indices_.clear();
  }

 private:
  RowBatch batch_;
  std::vector<std::shared_ptr<ColumnChunk>> packed_;
  std::vector<BufferPool<ColumnChunk>> buffers_;
  std::vector<BufferPool<FlexVector<uint32_t>>> index_pools_;
  std::vector<std::shared_ptr<FlexVector<uint32_t>>> indices_;
};

}  // namespace perfetto::trace_processor::core::exec
#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_BUFFER_H_
