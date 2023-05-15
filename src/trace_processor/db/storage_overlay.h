/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_OVERLAY_H_

#include <variant>
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column_overlay.h"
#include "src/trace_processor/db/storage.h"

namespace perfetto {
namespace trace_processor {
namespace column {

// Overlay responsible for doing operations on storage.
class StorageOverlay : public ColumnOverlay {
 public:
  explicit StorageOverlay(const Storage* storage) : storage_(storage) {}
  void Filter(FilterOp, SqlValue, RowMap&) override;
  void StableSort(uint32_t* rows, uint32_t rows_size) override;

 private:
  const Storage* storage_;
};
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_OVERLAY_H_
