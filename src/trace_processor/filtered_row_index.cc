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

#include <numeric>

namespace perfetto {
namespace trace_processor {

FilteredRowIndex::FilteredRowIndex(uint32_t start_row, uint32_t end_row)
    : mode_(Mode::kAllRows), start_row_(start_row), end_row_(end_row) {}

void FilteredRowIndex::IntersectRows(std::vector<uint32_t> rows) {
  PERFETTO_DCHECK(error_.empty());

  // Sort the rows so all branches below make sense.
  std::sort(rows.begin(), rows.end());

  if (mode_ == kAllRows) {
    mode_ = Mode::kRowVector;
    // Yes you're reading this code correctly. We use lower_bound in both cases.
    // Yes this is very intentional and has to do with |end_row_| already being
    // one greater than the value we are searching for so we need the first
    // iterator which is *geq* the |end_row_|.
    auto begin = std::lower_bound(rows.begin(), rows.end(), start_row_);
    auto end = std::lower_bound(begin, rows.end(), end_row_);
    rows_.insert(rows_.end(), begin, end);
    return;
  } else if (mode_ == kRowVector) {
    std::vector<uint32_t> intersected;
    std::set_intersection(rows_.begin(), rows_.end(), rows.begin(), rows.end(),
                          std::back_inserter(intersected));
    rows_ = std::move(intersected);
    return;
  }

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

std::vector<uint32_t> FilteredRowIndex::ToRowVector() {
  PERFETTO_DCHECK(error_.empty());

  switch (mode_) {
    case Mode::kAllRows:
      mode_ = Mode::kRowVector;
      rows_.resize(end_row_ - start_row_);
      std::iota(rows_.begin(), rows_.end(), start_row_);
      break;
    case Mode::kBitVector:
      ConvertBitVectorToRowVector();
      break;
    case Mode::kRowVector:
      // Nothing to do.
      break;
  }
  return TakeRowVector();
}

void FilteredRowIndex::ConvertBitVectorToRowVector() {
  PERFETTO_DCHECK(error_.empty());

  mode_ = Mode::kRowVector;

  auto b = row_filter_.begin();
  auto e = row_filter_.end();
  using std::find;
  for (auto it = find(b, e, true); it != e; it = find(it + 1, e, true)) {
    auto filter_idx = static_cast<uint32_t>(std::distance(b, it));
    rows_.emplace_back(filter_idx + start_row_);
  }
  row_filter_.clear();
}

std::unique_ptr<RowIterator> FilteredRowIndex::ToRowIterator(bool desc) {
  PERFETTO_DCHECK(error_.empty());

  switch (mode_) {
    case Mode::kAllRows:
      return std::unique_ptr<RangeRowIterator>(
          new RangeRowIterator(start_row_, end_row_, desc));
    case Mode::kBitVector: {
      return std::unique_ptr<RangeRowIterator>(
          new RangeRowIterator(start_row_, desc, TakeBitVector()));
    }
    case Mode::kRowVector: {
      auto vector = TakeRowVector();
      if (desc)
        std::reverse(vector.begin(), vector.end());
      return std::unique_ptr<VectorRowIterator>(
          new VectorRowIterator(std::move(vector)));
    }
  }
  PERFETTO_FATAL("For GCC");
}

std::vector<uint32_t> FilteredRowIndex::TakeRowVector() {
  PERFETTO_DCHECK(error_.empty());

  PERFETTO_DCHECK(mode_ == Mode::kRowVector);
  auto vector = std::move(rows_);
  rows_.clear();
  mode_ = Mode::kAllRows;
  return vector;
}

std::vector<bool> FilteredRowIndex::TakeBitVector() {
  PERFETTO_DCHECK(error_.empty());

  PERFETTO_DCHECK(mode_ == Mode::kBitVector);
  auto filter = std::move(row_filter_);
  row_filter_.clear();
  mode_ = Mode::kAllRows;
  return filter;
}

}  // namespace trace_processor
}  // namespace perfetto
