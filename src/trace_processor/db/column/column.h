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
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_COLUMN_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_COLUMN_H_

#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto {
namespace protos::pbzero {
class SerializedColumn_Storage;
}

namespace trace_processor {
namespace column {

// Defines an API of a Column. Storages and Overlays both inherit from
// Column.
class Column {
 public:
  using StorageProto = protos::pbzero::SerializedColumn_Storage;

  virtual ~Column();

  // Verifies whether any further filtering is needed and if not, whether the
  // search would return all values or none of them. This allows for skipping
  // the |Search| and |IndexSearch| in special cases.
  //
  // Notes for callers:
  // * The SqlValue and FilterOp have to be valid in Sqlite: it will crash if
  //   either: value is NULL and operation is different than "IS NULL" and "IS
  //   NOT NULL" or the operation is "IS NULL" and "IS NOT NULL" and value is
  //   different than NULL.
  virtual SearchValidationResult ValidateSearchConstraints(SqlValue,
                                                           FilterOp) const = 0;

  // Searches for elements which match |op| and |value| between |range.start|
  // and |range.end|.
  //
  // Returns either a range or BitVector which indicate the positions in |range|
  // which match the constraint. If a BitVector is returned, it will be
  // *precisely* as large as |range.end|.
  //
  // Notes for callers:
  //  * Should only be called if ValidateSearchContraints returned kOk.
  //  * Callers should note that the return value of this function corresponds
  //    to positions in the storage.
  //
  // Notes for implementors:
  //  * Implementations should ensure that the return value *only* includes
  //    positions in |range| as callers will expect this to be true and can
  //    optimize based on this.
  //  * Implementations should ensure that, if they return a BitVector, it is
  //    precisely of size |range.end|.
  virtual RangeOrBitVector Search(FilterOp, SqlValue, Range) const = 0;

  // Searches for elements which match |op| and |value| at the positions given
  // by |indices| array.
  //
  // Returns either a range of BitVector which indicate the positions in
  // |indices| which match the constraint. If a BitVector is returned, it will
  // be *precisely* as large as |indices_count|.
  //
  // Notes for callers:
  //  * Should only be called if ValidateSearchContraints returned kOk.
  //  * Callers should note that the return value of this function corresponds
  //    to positions in |indices| *not* positions in the storage.
  //
  // Notes for implementors:
  //  * Implementations should ensure that, if they return a BitVector, it is
  //    precisely of size |indices_count|.
  virtual RangeOrBitVector IndexSearch(FilterOp, SqlValue, Indices) const = 0;

  // Searches for elements which match |op| and |value| at the positions given
  // by indices data.
  //
  // Returns a Range into Indices data of indices that pass the constraint.
  //
  // Notes for callers:
  //  * Should not be called on:
  //    - kGlob and kRegex as those operations can't use the sorted state hence
  //      they can't return a Range.
  //    - kNe as this is inherently unsorted. Use kEq and then reverse the
  //      result.
  //  * Should only be called if ValidateSearchContraints returned kOk.
  //  * Callers should note that the return value of this function corresponds
  //    to positions in |indices| *not* positions in the storage.
  virtual Range OrderedIndexSearch(FilterOp, SqlValue, Indices) const = 0;

  // Sorts |rows| in ascending order with the comparator:
  // data[rows[a]] < data[rows[b]].
  virtual void Sort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Stable sorts |rows| in ascending order with the comparator:
  // data[rows[a]] < data[rows[b]].
  virtual void StableSort(uint32_t* rows, uint32_t rows_size) const = 0;

  // Serializes storage data to proto format.
  virtual void Serialize(StorageProto*) const = 0;

  // Number of elements in stored data.
  virtual uint32_t size() const = 0;
};

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_COLUMN_H_
