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
  virtual RangeOrBitVector Search(FilterOp op,
                                  SqlValue value,
                                  RowMap::Range range) const = 0;

  // Searches for elements which match |op| and |value| at the positions given
  // by |indices| array.
  // If the order defined by |indices| makes storage sorted, |sorted| flag
  // should be set to true.
  virtual RangeOrBitVector IndexSearch(FilterOp op,
                                       SqlValue value,
                                       uint32_t* indices,
                                       uint32_t indices_count,
                                       bool sorted = false) const = 0;

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
