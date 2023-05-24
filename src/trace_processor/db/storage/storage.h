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
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/storage/types.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

// Backing storage for columnar tables.
class Storage {
 public:
  virtual ~Storage();

  // Searches for elements which match |op| and |value| between |range.start|
  // and |range.end|.
  //
  // Returns a BitVector of size |range.end| with the position of the 1s
  // representing the positions which matched and 0s otherwise. The first
  // |range.start| number of elements will be zero.
  virtual BitVector LinearSearch(FilterOp op,
                                 SqlValue value,
                                 RowMap::Range range) const = 0;

  // Searches for elements which match |op| and |value| at the positions given
  // by |indices| array.
  //
  // Returns a BitVector of size |indices_count| with the position of the 1s
  // representing the positions which matched and 0s otherwise.
  virtual BitVector IndexSearch(FilterOp op,
                                SqlValue value,
                                uint32_t* indices,
                                uint32_t indices_count) const = 0;

  // Binary searches for elements which match |op| and |value| between
  // |range.start_index| and |range.end_index|.
  //
  // Returns a range, indexing the storage, where all elements in that range
  // match the constraint.
  //
  // Note: the caller *must* know that the elements in this storage are sorted;
  // it is an error to call this method otherwise.
  virtual RowMap::Range BinarySearchIntrinsic(FilterOp op,
                                              SqlValue value,
                                              RowMap::Range range) const = 0;

  // Binary searches for elements which match |op| and |value| only considering
  // the elements in |indices|.
  //
  // Returns a sub-Range of Range[0, indices_count) which indicates the
  // positions of elements in |indices| which match.
  //
  // Note: the caller *must* known that the elements in storage will be sorted
  // by the elements in |indices|; it is undefined behaviour to call this method
  // otherwise.
  virtual RowMap::Range BinarySearchExtrinsic(FilterOp op,
                                              SqlValue value,
                                              uint32_t* indices,
                                              uint32_t indices_count) const = 0;

  // Sorts |rows| in ascending order with the comparator:
  // data[rows[a]] < data[rows[b]].
  virtual void Sort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Stable sorts |rows| in ascending order with the comparator:
  // data[rows[a]] < data[rows[b]].
  virtual void StableSort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Number of elements in stored data.
  virtual uint32_t size() const = 0;
};

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_STORAGE_STORAGE_H_
