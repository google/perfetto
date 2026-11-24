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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_LAYOUT_COLUMN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_LAYOUT_COLUMN_H_

#include <cstdint>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {

// Writes `column`'s value at each row `selection` keeps into `slot` of the
// rows laid out at `rows`, one per row kept. `value(index)` converts the value
// stored at `index` into a T.
template <typename T, typename Value>
PERFETTO_ALWAYS_INLINE void WriteLayoutColumn(const ColumnView& column,
                                              const Selection& selection,
                                              RowLayout::Slot slot,
                                              uint8_t* rows,
                                              Value value) {
  uint32_t start = column.start();
  const BitVector* validity = column.validity();
  RowLayout::Write<T>(
      slot, selection.size(),
      [&](uint32_t row, T* out) {
        uint32_t index = start + selection[row];
        if (validity && !validity->is_set(index)) {
          return false;
        }
        *out = value(index);
        return true;
      },
      rows);
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_LAYOUT_COLUMN_H_
