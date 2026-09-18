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

#include <algorithm>
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
  selections_.Reset();
  auto block = selections_.TakeBlock();
  std::copy(rows.begin(), rows.end(), block->data());
  for (ColumnView& column : columns_) {
    column.SetOwnedRows(block, count);
  }
  cardinality_ = count;
  return true;
}

void RowBatch::Compose(RowSelection selection, uint32_t count) {
  selections_.Reset();
  compositions_.clear();
  for (uint32_t c = 0; c < columns_.size(); ++c) {
    auto& column = columns_[c];
    RowSelection current = column.selection();
    auto found = std::find_if(compositions_.begin(), compositions_.end(),
                              [&](const auto& entry) {
                                return entry.first.data() == current.data() &&
                                       entry.first.offset() == current.offset();
                              });
    if (found != compositions_.end()) {
      column.AdoptSelection(columns_[found->second]);
    } else {
      compositions_.emplace_back(current, c);
      column.Slice(selection, count, selections_);
    }
  }
}

bool RowBatch::Slice(RowSelection selection, uint32_t count) {
  PERFETTO_DCHECK(count <= kMaxBatchRows);
#if PERFETTO_DCHECK_IS_ON()
  for (uint32_t row = 0; row < count; ++row) {
    PERFETTO_DCHECK(selection.GetIndex(row) < cardinality_);
  }
#endif
  if (count == 0) {
    cardinality_ = 0;
    return false;
  }
  if (count == cardinality_ && selection.is_range() &&
      selection.offset() == 0) {
    return true;
  }
  Compose(selection, count);
  cardinality_ = count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
