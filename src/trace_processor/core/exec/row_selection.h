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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_

#include <cstdint>

#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// A lightweight view of logical-to-physical row indices. Indexed selections
// do not own their storage.
class RowSelection {
 public:
  static RowSelection Range(uint32_t offset = 0) {
    return RowSelection(nullptr, offset);
  }
  static RowSelection Indices(Span<const uint32_t> rows) {
    return RowSelection(rows.data(), 0);
  }

  uint32_t GetIndex(uint32_t row) const {
    return rows_ ? rows_[row] : offset_ + row;
  }
  bool is_range() const { return rows_ == nullptr; }
  const uint32_t* data() const { return rows_; }
  uint32_t offset() const { return offset_; }

 private:
  RowSelection(const uint32_t* rows, uint32_t offset)
      : rows_(rows), offset_(offset) {}

  const uint32_t* rows_;
  uint32_t offset_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_
