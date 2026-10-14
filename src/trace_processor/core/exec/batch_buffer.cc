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

#include "src/trace_processor/core/exec/batch_buffer.h"

#include <cstdint>

namespace perfetto::trace_processor::core::exec {

base::Status BatchBuffer::Append(const RowBatch& in, Context& context) {
  if (!in.size())
    return base::OkStatus();
  uint32_t before = size_, total = before + in.size();
  if (before) {
    if (total > kMaxBatchRows || in.column_count() != columns_.size())
      return base::ErrStatus("batch buffer: incompatible batch size or schema");
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      if (!SameLogicalType(columns_[c].view, in.column(c)))
        return base::ErrStatus("batch buffer: column representation changed");
    }
  } else {
    columns_.resize(in.column_count());
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      columns_[c].view = in.column(c);
    }
  }
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    Column& column = columns_[c];
    if (!column.packed) {
      column.packed = context.TakeBuffer();
    }
    column.packed.chunk().CopyFrom(in, c, before);
    column.nullable |= in.column(c).validity() != nullptr;
  }
  size_ = total;
  return base::OkStatus();
}

void BatchBuffer::Take(RowBatch& out) {
  out.Reset();
  for (Column& column : columns_) {
    // Viewed before the buffer is moved: arguments may be evaluated in any
    // order.
    ColumnView view = column.packed.chunk().View(column.view, column.nullable);
    out.AddColumn(view, std::move(column.packed));
  }
  out.SetRowCount(size_);
  Clear();
}

}  // namespace perfetto::trace_processor::core::exec
