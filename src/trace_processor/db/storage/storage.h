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
#ifndef SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_H_

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

using Range = RowMap::Range;

// Most base column interpreting layer - responsible for implementing searches
// and sorting.
class Storage {
 public:
  virtual ~Storage();

  // Changes the vector of indices to represent the sorted (stable sort) state
  // of the column.
  virtual void StableSort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Changes the vector of indices to represent the sorted (not stable) state of
  // the column.
  virtual void Sort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Efficiently compares series of |num_elements| of data from |data_start| to
  // comparator value and appends results to BitVector::Builder. Should be used
  // where possible
  virtual void LinearSearchAligned(FilterOp op,
                                   SqlValue value,
                                   uint32_t offset,
                                   uint32_t compare_elements_count,
                                   BitVector::Builder&) const = 0;

  // Inefficiently compares series of |num_elements| of data from |data_start|
  // to comparator value and appends results to BitVector::Builder. Should be
  // avoided if possible, with `LinearSearchAligned` used instead.
  virtual void LinearSearchUnaligned(FilterOp op,
                                     SqlValue value,
                                     uint32_t offset,
                                     uint32_t compare_elements_count,
                                     BitVector::Builder&) const = 0;

  // Compares sorted (asc) series data in |range| with comparator value. Should
  // be used where possible. Returns the Range of indices which match the
  // constraint.
  virtual std::optional<Range> BinarySearch(FilterOp op,
                                            SqlValue value,
                                            Range search_range) const = 0;

  // Compares sorted (asc) with |order| vector series in |range| with comparator
  // value. Should be used where possible. Returns the Range of indices
  // inside |order| vector which match the constraint.
  virtual std::optional<Range> BinarySearchWithIndex(
      FilterOp op,
      SqlValue value,
      uint32_t* order,
      Range search_range) const = 0;

  // Number of elements in stored data.
  virtual uint32_t size() const = 0;
};

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_H_
