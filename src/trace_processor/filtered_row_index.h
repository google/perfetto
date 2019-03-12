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
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/row_iterators.h"

namespace perfetto {
namespace trace_processor {

// Storage for information about the rows to be returned by a filter operation.
//
// There are two users of FilteredRowIndex:
// 1. The filter classes which get told to filter all their rows on a certain
// constraint, returning their result in an instance of this class. They will
// have to call one of the three functions described below to restrict the rows
// returned.
// 2. The coordiator which has access to all the constraints. They pass an
// instance of this class and the constraint to filter on to each filter
// class. Once all the constraints are filtered, the coordinator will extract
// the underlying row representation from the instance of this class and use
// that to read rows from the storage.
class FilteredRowIndex {
 public:
  FilteredRowIndex(uint32_t start_row, uint32_t end_row);

  // One of the following three functions can be called by the filter classes
  // to restrict which rows should be returned.

  // Interesects the rows specified by |rows| with the already filtered rows
  // and updates the index to the intersection.
  void IntersectRows(std::vector<uint32_t> rows);

  // Calls |fn| on each row index which is currently to be returned and retains
  // row index if |fn| returns true or discards the row otherwise.
  template <typename RowPredicate /* (uint32_t) -> bool */>
  void FilterRows(RowPredicate fn) {
    PERFETTO_DCHECK(error_.empty());

    switch (mode_) {
      case Mode::kAllRows:
        FilterAllRows(fn);
        break;
      case Mode::kBitVector:
        FilterBitVector(fn);
        break;
      case Mode::kRowVector:
        FilterRowVector(fn);
        break;
    }
  }

  // Called when there is some error in the filter operation requested. The
  // error string is used by the coordinator to report the error to SQLite.
  void set_error(std::string error) { error_ = std::move(error); }

  // The following functions should only be called by the coordinator classes.

  // Converts this index into a vector of row indicies.
  // Note: this function leaves the index in a freshly constructed state.
  std::vector<uint32_t> ToRowVector();

  // Converts this index into a row iterator.
  // Note: this function leaves the index in a freshly constructed state.
  std::unique_ptr<RowIterator> ToRowIterator(bool desc);

  // Returns the error from the filter operation invoked. May be empty if no
  // error occurred.
  const std::string& error() const { return error_; }

 private:
  enum Mode {
    kAllRows = 1,
    kBitVector = 2,
    kRowVector = 3,
  };

  template <typename Predicate>
  void FilterAllRows(Predicate fn) {
    mode_ = Mode::kBitVector;
    row_filter_.resize(end_row_ - start_row_, false);

    for (auto i = start_row_; i < end_row_; i++) {
      if (fn(i))
        row_filter_[i - start_row_] = true;
    }
  }

  template <typename Predicate>
  void FilterBitVector(Predicate fn) {
    auto b = row_filter_.begin();
    auto e = row_filter_.end();

    using std::find;
    for (auto it = find(b, e, true); it != e; it = find(it + 1, e, true)) {
      auto filter_idx = static_cast<uint32_t>(std::distance(b, it));
      auto value_it = start_row_ + filter_idx;
      *it = fn(value_it);
    }
  }

  template <typename Predicate>
  void FilterRowVector(Predicate fn) {
    size_t rows_size = rows_.size();
    for (size_t i = 0; i < rows_size;) {
      if (fn(rows_[i])) {
        i++;
      } else {
        std::swap(rows_[i], rows_[rows_size - 1]);
        rows_size--;
      }
    }
    rows_.resize(rows_size);
  }

  void ConvertBitVectorToRowVector();

  std::vector<uint32_t> TakeRowVector();

  std::vector<bool> TakeBitVector();

  Mode mode_;
  uint32_t start_row_;
  uint32_t end_row_;

  // Only non-empty when |mode_| == Mode::kBitVector.
  std::vector<bool> row_filter_;

  // Only non-empty when |mode_| == Mode::kRowVector.
  // This vector is sorted.
  std::vector<uint32_t> rows_;

  std::string error_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_FILTERED_ROW_INDEX_H_
