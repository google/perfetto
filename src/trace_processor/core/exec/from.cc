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

#include "src/trace_processor/core/exec/from.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/transient_column.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

From::From(std::vector<TransientColumn> columns,
           RowSelection rows,
           uint32_t count)
    : columns_(std::move(columns)), rows_(rows), count_(count) {
  for (const TransientColumn& column : columns_) {
    batch_.AddColumn(column);
  }
}

RowBatch* From::Next() {
  if (emitted_ == count_) {
    return nullptr;
  }
  uint32_t count = std::min(kMaxBatchRows, count_ - emitted_);
  RowSelection selection = RowSelection::Range(rows_.GetIndex(emitted_));
  if (!rows_.is_range()) {
    const uint32_t* begin = rows_.data() + emitted_;
    selection =
        RowSelection::Indices(Span<const uint32_t>(begin, begin + count));
  }
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    TransientColumn& column = batch_.mutable_column(i);
    // Operators only ever replace a column's row view, so restoring that view
    // is all a previously used batch needs before it is filled again.
    column.AdoptSelection(columns_[i]);
    column.Slice(selection, count);
  }
  batch_.SetCardinality(count);
  emitted_ += count;
  return &batch_;
}

}  // namespace perfetto::trace_processor::core::exec
