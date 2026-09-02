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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec::test {

template <typename T>
std::vector<T> ReadColumn(const RowBatch& batch, uint32_t column) {
  const ColumnView& view = batch.column(column);
  std::vector<T> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    values.push_back(view.Value<T>(row));
  }
  return values;
}

template <typename T>
std::vector<std::optional<T>> ReadNullableColumn(const RowBatch& batch,
                                                 uint32_t column) {
  const ColumnView& view = batch.column(column);
  const BitVector* validity = view.validity();
  std::vector<std::optional<T>> values;
  values.reserve(batch.size());
  for (uint32_t row = 0; row < batch.size(); ++row) {
    uint32_t index = view.selection().GetIndex(row);
    if (validity && !validity->is_set(index)) {
      values.emplace_back(std::nullopt);
    } else {
      values.emplace_back(view.Value<T>(row));
    }
  }
  return values;
}

}  // namespace perfetto::trace_processor::core::exec::test

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TEST_UTILS_H_
