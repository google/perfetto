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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_DATA_LAYER_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_DATA_LAYER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::protos::pbzero {
class SerializedColumn_Storage;
}

namespace perfetto::trace_processor::column {
class DataLayerChain;

// Data structure which either directly or indirectly (i.e. by transforming
// the contents of another DataLayer) provides the data of a column of a
// table.
class DataLayer : public RefCounted {
 public:
  virtual ~DataLayer();

  // Creates a DataLayerChain for a terminal DataLayer. This means the
  // DataLayer directly should return the data it contains inside.
  virtual std::unique_ptr<DataLayerChain> MakeChain() {
    PERFETTO_FATAL("Unimplemented");
  }

  // Creates a DataLayerChain for a non-terminal DataLayer. This means
  // the DataLayer should transform the contents of the inner chain.
  virtual std::unique_ptr<DataLayerChain> MakeChain(
      std::unique_ptr<DataLayerChain>) {
    PERFETTO_FATAL("Unimplemented");
  }
};

// Corresponds to a series of DataLayer chained together. Provides
// functionality for querying the transformed data of the entire chain.
class DataLayerChain {
 public:
  using StorageProto = protos::pbzero::SerializedColumn_Storage;

  virtual ~DataLayerChain();

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
  // Returns either a range or BitVector which indicate the positions in
  // |range| which match the constraint. If a BitVector is returned, it will
  // be *precisely* as large as |range.end|.
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
  //    - kGlob and kRegex as those operations can't use the sorted state
  //      hence they can't return a Range.
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

  // Returns a string which represents the column for debugging purposes.
  //
  // Warning: the format of the string returned by this class is *not* stable
  // and should be relied upon for anything except printing for debugging
  // purposes.
  virtual std::string DebugString() const = 0;

  // Number of elements in stored data.
  virtual uint32_t size() const = 0;
};

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_DATA_LAYER_H_
