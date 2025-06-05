/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/winscope_transform.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

WinscopeTransformTracker::WinscopeTransformTracker(
    TraceProcessorContext* context)
    : context_(context) {}

WinscopeTransformTracker::~WinscopeTransformTracker() = default;

tables::WinscopeTransformTable::Id* WinscopeTransformTracker::GetOrInsertRow(
    const TransformMatrix& matrix) {
  auto existing_matrix =
      std::find_if(rows_.begin(), rows_.end(),
                   [&](const Row& r) { return r.matrix == matrix; });
  if (existing_matrix != rows_.end()) {
    return &existing_matrix->row_id;
  }

  tables::WinscopeTransformTable::Row row;
  row.dsdx = matrix.dsdx;
  row.dtdx = matrix.dtdx;
  row.dsdy = matrix.dsdy;
  row.dtdy = matrix.dtdy;
  row.tx = matrix.tx;
  row.ty = matrix.ty;
  auto id =
      context_->storage->mutable_winscope_transform_table()->Insert(row).id;

  rows_.push_back({id, matrix});

  auto& new_row = rows_[rows_.size() - 1];
  return &(new_row.row_id);
}

}  // namespace trace_processor
}  // namespace perfetto
