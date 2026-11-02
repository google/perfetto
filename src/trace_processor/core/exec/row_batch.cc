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
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"

namespace perfetto::trace_processor::core::exec {

RowBatch& RowBatch::operator=(RowBatch&& other) noexcept {
  own_columns_ = std::move(other.own_columns_);
  own_buffers_ = std::move(other.own_buffers_);
  if (other.owned_) {
    Repoint();
  } else {
    SetColumns(other.columns_, other.column_count_, other.buffers_,
               other.buffer_count_);
  }
  selection_.CopyFrom(other.selection_);
  other.Reset();
  return *this;
}

void RowBatch::CopyFrom(const RowBatch& other) {
  PERFETTO_DCHECK(&other != this);
  own_columns_.assign(other.columns_, other.columns_ + other.column_count_);
  own_buffers_.assign(other.buffers_, other.buffers_ + other.buffer_count_);
  Repoint();
  selection_.CopyFrom(other.selection_);
}

void RowBatch::Project(const RowBatch& other,
                       const std::vector<uint32_t>& columns) {
  Reset();
  for (uint32_t column : columns) {
    AddColumn(other.column(column), other.buffer(column));
  }
  selection_.CopyFrom(other.selection_);
}

void RowBatch::AddColumn(const ColumnView& view, ColumnBuffer buffer) {
  Own();
  own_columns_.push_back(view);
  own_buffers_.push_back(std::move(buffer));
  Repoint();
}

void RowBatch::SetColumn(uint32_t column,
                         const ColumnView& view,
                         ColumnBuffer buffer) {
  Own();
  own_columns_[column] = view;
  own_buffers_[column] = std::move(buffer);
  Repoint();
}

void RowBatch::Reset() {
  own_columns_.clear();
  own_buffers_.clear();
  Repoint();
  selection_.Reset(0);
}

void RowBatch::Own() {
  if (!owned_) {
    own_columns_.assign(columns_, columns_ + column_count_);
    own_buffers_.assign(buffers_, buffers_ + buffer_count_);
    Repoint();
  }
}

}  // namespace perfetto::trace_processor::core::exec
