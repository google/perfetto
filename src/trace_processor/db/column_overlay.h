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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_OVERLAY_H_

#include <variant>
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/storage.h"

namespace perfetto {
namespace trace_processor {
namespace column {

// Column overlay introduce separation between column storage (vector of data)
// and state (nullability, sorting) and actions (filtering, expanding, joining)
// done on the storage. This is a composable design - one ColumnOverlay
// subclass might hold another subclass, and each of them implements all of the
// functions in it's own specific way.
class ColumnOverlay {
 public:
  virtual ~ColumnOverlay();

  // Clears the rows of RowMap, on which data don't match the FilterOp operation
  // with SqlValue. Efficient.
  virtual void Filter(FilterOp, SqlValue, RowMap&) = 0;

  // Sorts (ascending) provided vector of indices based on storage.
  virtual void Sort(std::vector<uint32_t>&) = 0;
};
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_OVERLAY_H_
