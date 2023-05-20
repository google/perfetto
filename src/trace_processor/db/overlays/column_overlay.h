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

#ifndef SRC_TRACE_PROCESSOR_DB_OVERLAYS_COLUMN_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_OVERLAYS_COLUMN_OVERLAY_H_

#include <variant>
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/storage.h"

namespace perfetto {
namespace trace_processor {
namespace column {

enum class SearchAlgorithm {
  kLinearSearch,
  kBinarySearch,
};

// Column overlay introduce separation between column storage (vector of data)
// and state (nullability, sorting) and actions (filtering, expanding, joining)
// done on the storage. This is a composable design - one Overlay is
// DecidingSearchAlgorithm based on the result of the same function from
// previous Overlay.
class ColumnOverlay {
 public:
  virtual ~ColumnOverlay();

  // Returns the RowMap without information added by overlay. The result would
  // be used by the overlay one step closer to Storage. For example, for
  // NullOverlay it would be returning RowMap that would be only of the length
  // of set bits in a bit vector.
  virtual RowMap TranslateDown(RowMap) const = 0;

  // Returns the RowMap with information added by overlay. The result would be
  // used by the overlay one step closer to the Table. For example, for
  // NullOverlay it would be returning RowMap that would be the length of bit
  // vector, nulls included.
  virtual RowMap TranslateUp(RowMap) const = 0;

  // Decides what search algorithm should be called based on the type of overlay
  // and passed SearchAlgorithm.
  virtual SearchAlgorithm DecideSearchAlgorithm(SearchAlgorithm) const = 0;
};
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_OVERLAYS_COLUMN_OVERLAY_H_
