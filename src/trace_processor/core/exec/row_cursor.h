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
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// One execution of a plan, read a row at a time. The executor: it owns the
// state and the batch, which is why the plan can be const. Batches are pulled
// only when the current one runs out.
class RowCursor {
 public:
  explicit RowCursor(const Source& source);
  ~RowCursor();

  // Rewinds, then reads up to the first row.
  bool Open() {
    source_.Rewind(*state_);
    index_ = 0;
    size_ = 0;
    return Pull();
  }

  // Reads up to the next row, pulling a batch if this one is exhausted.
  bool Next() { return ++index_ != size_ || Pull(); }

  bool eof() const { return index_ == size_; }

  base::Status status() const { return source_.status(*state_); }

  // The physical row `column` is read at. Per column, because a computed
  // column sits in its own index space rather than its input's.
  PERFETTO_ALWAYS_INLINE uint32_t row(uint32_t column) const {
    PERFETTO_DCHECK(index_ < size_);
    return batch_.column(column).selection().GetIndex(index_);
  }

  const RowBatch& batch() const { return batch_; }

 private:
  bool Pull();

  const Source& source_;
  std::unique_ptr<OperatorState> state_;
  RowBatch batch_;
  uint32_t index_ = 0;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_
