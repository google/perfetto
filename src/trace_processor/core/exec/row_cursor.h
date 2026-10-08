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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_

#include <cstdint>
#include <memory>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Reads one execution of a plan a row at a time.
//
// This is the executor: it creates the state and owns the batch and the
// Context the run fills columns from, which is what
// lets the plan itself be const. A new batch is pulled only when the current
// one is exhausted.
class RowCursor {
 public:
  explicit RowCursor(const Source& source);
  RowCursor(Source&&) = delete;
  ~RowCursor();

  // Rewinds and moves to the first row.
  bool Open() {
    source_.Rewind(*state_);
    batch_ = nullptr;
    index_ = 0;
    size_ = 0;
    return Pull();
  }

  // Moves to the next row, pulling a new batch if this one is exhausted. At
  // eof it stays put and keeps returning false.
  bool Next() {
    if (index_ == size_) {
      return false;
    }
    if (++index_ != size_) {
      row_ = batch_->selection()[index_];
      return true;
    }
    return !batch_->last() && Pull();
  }

  bool eof() const { return index_ == size_; }

  base::Status status() const { return source_.status(*state_); }

  // The value of `column` at the current row.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T Value(uint32_t column) const {
    PERFETTO_DCHECK(index_ < size_);
    return batch_->column(column).At<T>(row_);
  }

  const RowBatch& batch() const { return batch_ ? *batch_ : scratch_; }
  // Where the current row is in each of batch()'s columns.
  uint32_t row() const { return row_; }
  // Which batch batch() is, counting every batch pulled since the cursor was
  // made: a reader can keep what it worked out about a batch's columns until
  // this changes.
  uint32_t batch_number() const { return batch_number_; }

 private:
  bool Pull();

  const Source& source_;
  // Before the state and the batch, which hold its buffers.
  Context context_;
  std::unique_ptr<OperatorState> state_;
  RowBatch scratch_;
  // The source's current batch, valid until the next pull.
  RowBatch* batch_ = nullptr;
  // The current row: the `index_`th of the `size_` kept, at `row_` in the
  // columns.
  uint32_t index_ = 0;
  uint32_t size_ = 0;
  uint32_t row_ = 0;
  uint32_t batch_number_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_
