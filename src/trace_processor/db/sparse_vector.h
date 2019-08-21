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

#ifndef SRC_TRACE_PROCESSOR_DB_SPARSE_VECTOR_H_
#define SRC_TRACE_PROCESSOR_DB_SPARSE_VECTOR_H_

#include <stdint.h>

#include <deque>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/db/bit_vector.h"

namespace perfetto {
namespace trace_processor {

// A data structure which compactly stores a list of possibly nullable data.
//
// Internally, this class is implemented using a combination of a std::deque
// with a BitVector used to store whether each index is null or not.
// For each null value, it only uses a single bit inside the BitVector at
// a slight cost (searching the BitVector to find the index into the std::deque)
// when looking up the data.
template <typename T>
class SparseVector {
 public:
  // Creates an empty SparseVector.
  SparseVector() = default;

  // Returns the optional value at |idx| or base::nullopt if the value is null.
  base::Optional<T> Get(uint32_t idx) const {
    return valid_.IsSet(idx)
               ? base::Optional<T>(data_[valid_.GetNumBitsSet(idx)])
               : base::nullopt;
  }

  // Adds the given value to the SparseVector.
  void Append(T val) {
    data_.emplace_back(val);
    valid_.Append(true);
  }

  // Adds a null value to the SparseVector.
  void AppendNull() { valid_.Append(false); }

  // Sets the value at |idx| to the given |val|.
  void Set(uint32_t idx, T val) {
    uint32_t data_idx = valid_.GetNumBitsSet(idx);

    // Generally, we will be setting a null row to non-null so optimize for that
    // path.
    if (PERFETTO_UNLIKELY(valid_.IsSet(idx))) {
      data_[data_idx] = val;
    } else {
      data_.insert(data_.begin() + static_cast<ptrdiff_t>(data_idx), val);
      valid_.Set(idx, true);
    }
  }

  // Returns the size of the SparseVector; this includes any null values.
  uint32_t size() const { return valid_.size(); }

 private:
  explicit SparseVector(const SparseVector&) = delete;
  SparseVector& operator=(const SparseVector&) = delete;

  SparseVector(SparseVector&&) = delete;
  SparseVector& operator=(SparseVector&&) noexcept = delete;

  std::deque<T> data_;
  BitVector valid_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_SPARSE_VECTOR_H_
