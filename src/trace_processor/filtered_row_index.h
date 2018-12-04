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

#ifndef SRC_TRACE_PROCESSOR_FILTERED_ROW_INDEX_H_
#define SRC_TRACE_PROCESSOR_FILTERED_ROW_INDEX_H_

#include <stdint.h>
#include <algorithm>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// Storage for information about the rows to be returned by a filter operation.
class FilteredRowIndex {
 public:
  FilteredRowIndex(uint32_t start_row, uint32_t end_row);

  // Interesects the rows specified by |rows| with the already filtered rows
  // and updates the index to the intersection.
  void IntersectRows(std::vector<uint32_t> rows);

  // Cals |fn| on each row index which is currently to be returned and retains
  // row index if |fn| returns true or discards the row otherwise.
  template <typename Predicate>
  void FilterRows(Predicate fn) {
    switch (mode_) {
      case Mode::kAllRows:
        FilterAllRows(fn);
        break;
      case Mode::kBitVector:
        FilterBitVector(fn);
        break;
    }
  }

  // Returns the bitvector backing this row index. Rests the internal bitvector
  // storage as well.
  std::vector<bool> TakeBitvector() {
    if (mode_ == Mode::kAllRows)
      return std::vector<bool>(end_row_ - start_row_, true);

    auto vector = std::move(row_filter_);
    row_filter_.clear();
    mode_ = Mode::kAllRows;
    return vector;
  }

  bool all_set() { return mode_ == Mode::kAllRows; }
  uint32_t start_row() const { return start_row_; }
  uint32_t end_row() const { return end_row_; }

 private:
  enum Mode {
    kAllRows = 1,
    kBitVector = 2,
  };

  template <typename Predicate>
  void FilterAllRows(Predicate fn) {
    mode_ = Mode::kBitVector;
    row_filter_.resize(end_row_ - start_row_, true);

    for (uint32_t i = start_row_; i < end_row_; i++) {
      row_filter_[i - start_row_] = fn(i);
    }
  }

  template <typename Predicate>
  void FilterBitVector(Predicate fn) {
    auto b = row_filter_.begin();
    auto e = row_filter_.end();
    using std::find;
    for (auto it = find(b, e, true); it != e; it = find(it + 1, e, true)) {
      auto filter_idx = static_cast<uint32_t>(std::distance(b, it));
      *it = fn(start_row_ + filter_idx);
    }
  }

  Mode mode_;
  uint32_t start_row_;
  uint32_t end_row_;

  // Only non-empty when |mode_| == Mode::kBitVector.
  std::vector<bool> row_filter_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_FILTERED_ROW_INDEX_H_
