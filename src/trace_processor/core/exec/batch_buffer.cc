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
#include <utility>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {

void BatchBuffer::Append(const RowBatch& in, OperatorState& owner) {
  if (!in.size())
    return;
  uint32_t before = size_, total = before + in.size();
  // Callers remove rows before a batch overflows. The planner guarantees
  // every batch of a step has the same columns.
  PERFETTO_DCHECK(total <= kMaxBatchRows);
  if (before) {
    PERFETTO_DCHECK(in.column_count() == columns_.size());
#if PERFETTO_DCHECK_IS_ON()
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      PERFETTO_DCHECK(SameLogicalType(ColumnShape::Of(columns_[c].view),
                                      ColumnShape::Of(in.column(c))));
    }
#endif
  } else {
    columns_.resize(owner, in.column_count());
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      columns_[c] = {in.column(c), false};
    }
    if (in.column_count() > packed_capacity_) {
      packed_ = owner.context().TakeBuffers(owner, in.column_count());
      packed_capacity_ = in.column_count();
    }
  }
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    Column& column = columns_[c];
    ColumnBuffer& packed = packed_[c];
    if (!packed) {
      packed = owner.context().TakeBuffer(ValueWidth(in.column(c)));
    }
    packed.chunk().CopyFrom(in, c, before);
    column.nullable |= in.column(c).validity() != nullptr;
  }
  size_ = total;
}

void BatchBuffer::Take(RowBatch& out) {
  out.Reset();
  for (uint32_t c = 0; c < columns_.size(); ++c) {
    const Column& column = columns_[c];
    // Viewed before the buffer is moved: arguments may be evaluated in any
    // order.
    ColumnView view = packed_[c].chunk().View(column.view, column.nullable);
    out.AddColumn(view, std::move(packed_[c]));
  }
  out.SetRowCount(size_);
  Clear();
}

void BatchBuffer::Clear() {
  size_ = 0;
  for (uint32_t c = 0; c < columns_.size(); ++c) {
    packed_[c] = ColumnBuffer();
  }
  columns_.clear();
}

}  // namespace perfetto::trace_processor::core::exec
