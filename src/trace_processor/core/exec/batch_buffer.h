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

#include <cstdint>

#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"

namespace perfetto::trace_processor::core::exec {

// Combines small batches column by column, copying the rows each keeps into
// one buffer per column. Copying preserves duplicates, nulls and
// floating-point bits. String IDs refer to the process-lifetime pool; string
// payloads are not copied.
//
// Plain data, held for one run of the state `owner` which contains it. Its
// buffers are taken for that run (see Context::TakeBuffers()).
class BatchBuffer {
 public:
  uint32_t size() const { return size_; }
  // Copies the rows `in` keeps into buffers taken from the Context of `owner`.
  void Append(const RowBatch& in, OperatorState& owner);
  // Fills `out` with the rows combined, and empties the buffer.
  void Take(RowBatch& out);
  // Releases the combined rows.
  void Clear();

 private:
  struct Column {
    // The first batch's view, saying what the column holds.
    ColumnView view;
    bool nullable;
  };
  uint32_t size_ = 0;
  RunVector<Column> columns_;
  // By column, the rows combined so far, and how many slots `packed_` holds.
  ColumnBuffer* packed_ = nullptr;
  uint32_t packed_capacity_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec
#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_BUFFER_H_
