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
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Combines small batches column by column, copying the rows each keeps into
// one buffer per column. Copying preserves duplicates, nulls and
// floating-point bits. String IDs refer to the process-lifetime pool; string
// payloads are not copied.
class BatchBuffer {
 public:
  uint32_t size() const { return size_; }
  // Copies the rows `in` keeps, into buffers taken from `context`.
  base::Status Append(const RowBatch& in, Context& context);
  // Fills `out` with the rows combined, and empties the buffer.
  void Take(RowBatch& out);
  void Clear() {
    size_ = 0;
    for (auto& column : columns_) {
      column.packed = ColumnBuffer();
      column.nullable = false;
    }
  }

 private:
  struct Column {
    // The rows combined so far.
    ColumnBuffer packed;
    // The first batch's view, saying what the column holds.
    ColumnView view;
    bool nullable = false;
  };
  uint32_t size_ = 0;
  std::vector<Column> columns_;
};

}  // namespace perfetto::trace_processor::core::exec
#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_BUFFER_H_
