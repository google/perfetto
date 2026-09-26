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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_

#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Retains owned input without copying values. Range output shares the retained
// backing; arbitrary row-order output is gathered into contiguous column
// buffers so downstream consumers do not inherit scattered reads. Reordering
// operates only on rows already retained, without pulling additional input.
class RowStore {
 public:
  base::Status Append(const RowBatch&);
  uint32_t size() const { return size_; }
  uint32_t View(RowBatch* out, uint32_t offset, uint32_t count) const;
  // Gathers up to kMaxBatchRows in the requested order, including duplicates.
  // Output owns its buffers and survives later calls and store destruction.
  uint32_t View(RowBatch* out, Span<const uint32_t> rows);
  void Clear() {
    for (auto& column : columns_) {
      column.batches.clear();
      column.nullable = false;
      column.same_selection_as_previous = true;
    }
    ends_.clear();
    batch_of_row_.clear();
    size_ = 0;
  }

 private:
  uint32_t Find(uint32_t row) const;
  struct Column {
    struct Batch {
      ColumnView view;
      std::shared_ptr<const void> owner;
    };
    std::vector<Batch> batches;
    BufferPool<ColumnChunk> buffers;
    bool nullable = false;
    bool same_selection_as_previous = true;
  };
  std::vector<Column> columns_;
  std::vector<uint32_t> ends_;
  // Dense logical row numbers map directly to variable-sized input batches.
  std::vector<uint32_t> batch_of_row_;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec
#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
