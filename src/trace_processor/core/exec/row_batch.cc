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

#include "src/trace_processor/core/exec/row_batch.h"

#include <cstdint>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

bool RowBatch::AdoptPhysicalRows(Span<const uint32_t> rows) {
  auto count = static_cast<uint32_t>(rows.size());
  if (count == 0) {
    cardinality_ = 0;
    return false;
  }
  PERFETTO_DCHECK(count <= cardinality_);
  for (ColumnView& column : columns_) {
    column.SetBorrowedRows(rows);
  }
  cardinality_ = count;
  return true;
}

void RowBatch::Compose(RowSelection selection, uint32_t count) {
  // Columns filled by one source share a selection, and composing one is a
  // gather over the whole batch. Do it once per distinct selection and let the
  // other columns adopt the result.
  const ColumnView* composed = nullptr;
  const uint32_t* composed_from_rows = nullptr;
  uint32_t composed_from_offset = 0;
  for (ColumnView& column : columns_) {
    RowSelection current = column.selection();
    if (composed && current.data() == composed_from_rows &&
        current.offset() == composed_from_offset) {
      column.AdoptSelection(*composed);
      continue;
    }
    composed_from_rows = current.data();
    composed_from_offset = current.offset();
    column.Slice(selection, count, selections_);
    composed = &column;
  }
}

bool RowBatch::Slice(RowSelection selection, uint32_t count) {
  PERFETTO_DCHECK(count <= cardinality_);
  for (uint32_t row = 0; row < count; ++row) {
    PERFETTO_DCHECK(selection.GetIndex(row) < cardinality_);
    PERFETTO_DCHECK(row == 0 ||
                    selection.GetIndex(row - 1) < selection.GetIndex(row));
  }
  if (count == 0) {
    cardinality_ = 0;
    return false;
  }
  if (count == cardinality_) {
    return true;
  }
  Compose(selection, count);
  cardinality_ = count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
