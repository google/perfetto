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

#include "src/trace_processor/importers/proto/winscope/winscope_rect.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

WinscopeRectTracker::WinscopeRectTracker(TraceProcessorContext* context)
    : context_(context) {}

WinscopeRectTracker::~WinscopeRectTracker() = default;

tables::WinscopeRectTable::Id* WinscopeRectTracker::GetOrInsertRow(
    const WinscopeRect& rect) {
  auto existing_rect = std::find_if(
      rows_.begin(), rows_.end(), [&](const Row& r) { return r.rect == rect; });
  if (existing_rect != rows_.end()) {
    return &existing_rect->row_id;
  }

  tables::WinscopeRectTable::Row row;
  row.x = rect.x;
  row.y = rect.y;
  row.w = rect.w;
  row.h = rect.h;
  auto id = context_->storage->mutable_winscope_rect_table()->Insert(row).id;

  rows_.push_back({id, rect});

  auto& new_row = rows_[rows_.size() - 1];
  return &(new_row.row_id);
}

}  // namespace trace_processor
}  // namespace perfetto
