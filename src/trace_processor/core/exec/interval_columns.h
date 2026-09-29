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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_COLUMNS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_COLUMNS_H_

#include <cstdint>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// Reads one flat, non-null Int64 column of a batch.
struct Int64Reader {
  explicit Int64Reader(const ColumnView& column)
      : data(static_cast<const int64_t*>(column.data())),
        selection(column.selection()),
        validity(column.validity()) {}

  // False if the row holds no value.
  bool Read(uint32_t row, int64_t* out) const {
    uint32_t index = selection.GetIndex(row);
    if (validity && !validity->is_set(index)) {
      return false;
    }
    *out = data[index];
    return true;
  }

  const int64_t* data;
  RowSelection selection;
  const BitVector* validity;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_COLUMNS_H_
