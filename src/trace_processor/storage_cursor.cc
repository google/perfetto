/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/storage_cursor.h"

namespace perfetto {
namespace trace_processor {

StorageCursor::StorageCursor(std::unique_ptr<RowIterator> iterator,
                             std::vector<std::unique_ptr<StorageColumn>>* cols)
    : iterator_(std::move(iterator)), columns_(std::move(cols)) {}

int StorageCursor::Next() {
  iterator_->NextRow();
  return SQLITE_OK;
}

int StorageCursor::Eof() {
  return iterator_->IsEnd();
}

int StorageCursor::Column(sqlite3_context* context, int raw_col) {
  size_t column = static_cast<size_t>(raw_col);
  (*columns_)[column]->ReportResult(context, iterator_->Row());
  return SQLITE_OK;
}

}  // namespace trace_processor
}  // namespace perfetto
