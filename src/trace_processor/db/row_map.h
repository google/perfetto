/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_ROW_MAP_H_
#define SRC_TRACE_PROCESSOR_DB_ROW_MAP_H_

#include <stdint.h>

#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/db/bit_vector.h"

namespace perfetto {
namespace trace_processor {

// Stores a list of row indicies in a space efficient manner. One or more
// columns can refer to the same RowMap. The RowMap defines the access pattern
// to iterate on rows.
//
// Behind the scenes, this class is impelemented using one of two backing
// data-structures:
// 1. BitVector
// 2. std::vector<uint32_t>
//
// Generally a BitVector is used whenever possible because of its space
// efficiency compared to the small overhead of searching through the
// bitvector.
// However, as soon as sorting or duplicate rows come into play, we cannot use a
// BitVector anymore as ordering/duplicate row information cannot be captured by
// a BitVector. At this point, we switch to using an std::vector<uint32_t> and
// continue to do so even after the RowMap is modified to keep preserving
// ordering/duplicates.
class RowMap {
 public:
  // Creates a RowMap backed by a BitVector.
  explicit RowMap(BitVector bit_vector);

  // Creates a RowMap backed by an std::vector<uint32_t>.
  explicit RowMap(std::vector<uint32_t> vec);

  // Creates a copy of the RowMap.
  RowMap Copy() const;

  // Returns the size of the RowMap; that is the number of rows in the RowMap.
  uint32_t size() const {
    return compact_ ? bit_vector_.GetNumBitsSet()
                    : static_cast<uint32_t>(index_vector_.size());
  }

  // Returns the row at index |row|.
  uint32_t Get(uint32_t idx) const {
    PERFETTO_DCHECK(idx < size());
    return compact_ ? bit_vector_.IndexOfNthSet(idx) : index_vector_[idx];
  }

  // Returns the first index of the given |row| in the RowMap.
  uint32_t IndexOf(uint32_t row) const {
    if (compact_) {
      return bit_vector_.GetNumBitsSet(row);
    } else {
      auto it = std::find(index_vector_.begin(), index_vector_.end(), row);
      return static_cast<uint32_t>(std::distance(index_vector_.begin(), it));
    }
  }

  // Adds the given |row| to the RowMap.
  void Add(uint32_t row) {
    if (compact_) {
      if (row >= bit_vector_.size())
        bit_vector_.Resize(row + 1, false);
      bit_vector_.Set(row, true);
    } else {
      index_vector_.emplace_back(row);
    }
  }

  // Updates this RowMap by 'picking' the rows at indicies given by |picker|.
  // This is easiest to explain with an example; suppose we have the following
  // RowMaps:
  // this  : [0, 1, 4, 10, 11]
  // picker: [0, 3, 4, 4, 2]
  //
  // After calling Apply(picker), we now have the following:
  // this  : [0, 10, 11, 11, 4]
  //
  // Conceptually, we are performing the following algorithm:
  // for (idx : picker)
  //   this[i++] = this[idx]
  void SelectRows(const RowMap& picker);

  // Removes any row where |p(row)| returns false from this RowMap.
  template <typename Predicate>
  void RemoveIf(Predicate p) {
    if (compact_) {
      const auto& bv = bit_vector_;
      for (uint32_t i = bv.NextSet(0); i < bv.size(); i = bv.NextSet(i + 1)) {
        bit_vector_.Set(i, !p(i));
      }
    } else {
      auto it = std::remove_if(index_vector_.begin(), index_vector_.end(), p);
      index_vector_.erase(it, index_vector_.end());
    }
  }

 private:
  // TODO(lalitm): add a mode with two indicies marking out a range as well
  // or integrate this with BitVector.
  bool compact_ = false;

  // Only valid when |compact_| == true.
  BitVector bit_vector_;

  // Only valid when |compact_| == false.
  std::vector<uint32_t> index_vector_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_ROW_MAP_H_
