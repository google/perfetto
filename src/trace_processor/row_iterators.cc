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

#include "src/trace_processor/row_iterators.h"

#include <algorithm>

#include "src/trace_processor/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

template <typename Iterator>
uint32_t FindNextOffset(Iterator begin, Iterator end, uint32_t offset) {
  auto prev_it = begin + static_cast<ptrdiff_t>(offset);
  auto current_it = std::find(prev_it, end, true);
  return static_cast<uint32_t>(std::distance(begin, current_it));
}

uint32_t FindNextOffset(const std::vector<bool>& filter,
                        uint32_t offset,
                        bool desc) {
  if (desc)
    return FindNextOffset(filter.rbegin(), filter.rend(), offset);
  return FindNextOffset(filter.begin(), filter.end(), offset);
}

}  // namespace

RowIterator::~RowIterator() = default;

RangeRowIterator::RangeRowIterator(uint32_t start_row,
                                   uint32_t end_row,
                                   bool desc)
    : start_row_(start_row), end_row_(end_row), desc_(desc) {}

RangeRowIterator::RangeRowIterator(uint32_t start_row,
                                   bool desc,
                                   std::vector<bool> row_filter)
    : start_row_(start_row),
      end_row_(start_row_ + static_cast<uint32_t>(row_filter.size())),
      desc_(desc),
      row_filter_(std::move(row_filter)) {
  if (start_row_ < end_row_)
    offset_ = FindNextOffset(row_filter_, offset_, desc_);
}

void RangeRowIterator::NextRow() {
  PERFETTO_DCHECK(!IsEnd());
  offset_++;

  if (!row_filter_.empty())
    offset_ = FindNextOffset(row_filter_, offset_, desc_);
}

bool RangeRowIterator::IsEnd() {
  return offset_ >= end_row_ - start_row_;
}

uint32_t RangeRowIterator::Row() {
  return desc_ ? end_row_ - offset_ - 1 : start_row_ + offset_;
}

uint32_t RangeRowIterator::RowCount() const {
  if (row_filter_.empty()) {
    return end_row_ - start_row_;
  }
  auto count = std::count(row_filter_.begin(), row_filter_.end(), true);
  return static_cast<uint32_t>(count);
}

VectorRowIterator::VectorRowIterator(std::vector<uint32_t> row_indices)
    : row_indices_(std::move(row_indices)) {}
VectorRowIterator::~VectorRowIterator() = default;

void VectorRowIterator::NextRow() {
  offset_++;
}

bool VectorRowIterator::IsEnd() {
  return offset_ >= row_indices_.size();
}

uint32_t VectorRowIterator::Row() {
  return row_indices_[offset_];
}

}  // namespace trace_processor
}  // namespace perfetto
