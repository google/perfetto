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

#include "src/trace_processor/filtered_row_index.h"

namespace perfetto {
namespace trace_processor {

FilteredRowIndex::FilteredRowIndex(uint32_t start_row, uint32_t end_row)
    : mode_(Mode::kAllRows), start_row_(start_row), end_row_(end_row) {}

void FilteredRowIndex::IntersectRows(std::vector<uint32_t> rows) {
  if (mode_ == kAllRows) {
    mode_ = Mode::kBitVector;
    row_filter_.resize(end_row_ - start_row_, false);

    for (size_t row : rows) {
      // If a row is out of bounds of of the index, simply ignore it.
      if (row < start_row_ || row >= end_row_)
        continue;
      row_filter_[row - start_row_] = true;
    }
    return;
  }

  // Sort the rows so that the algorithm below makes sense.
  std::sort(rows.begin(), rows.end());

  // Initialise start to the beginning of the vector.
  auto start = row_filter_.begin();

  // Skip directly to the rows in range of start and end.
  size_t i = 0;
  for (; i < rows.size() && rows[i] < start_row_; i++) {
  }
  for (; i < rows.size() && rows[i] < end_row_; i++) {
    // Unset all bits between the start iterator and the iterator pointing
    // to the current row. That is, this loop sets all elements not pointed
    // to by rows to false. It does not touch the rows themselves which
    // means if they were already false (i.e. not returned) then they won't
    // be returned now and if they were true (i.e. returned) they will still
    // be returned.
    auto row = rows[i];
    auto end = row_filter_.begin() + static_cast<ptrdiff_t>(row - start_row_);
    std::fill(start, end, false);
    start = end + 1;
  }
  std::fill(start, row_filter_.end(), false);
}

}  // namespace trace_processor
}  // namespace perfetto
