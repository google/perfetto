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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_NULLABLE_VECTOR_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_NULLABLE_VECTOR_H_

#include <stdint.h>

#include <deque>
#include <optional>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto {
namespace trace_processor {

// A data structure which compactly stores a list of possibly nullable data.
//
// Internally, this class is implemented using a combination of a std::deque
// with a BitVector used to store whether each index is null or not.
// By default, for each null value, it only uses a single bit inside the
// BitVector at a slight cost (searching the BitVector to find the index into
// the std::deque) when looking up the data.
template <typename T>
class NullableVector {
 private:
  enum class Mode {
    // Sparse mode is the default mode and ensures that nulls are stored using
    // only
    // a single bit (at the cost of making setting null entries to non-null
    // O(n)).
    kSparse,

    // Dense mode forces the reservation of space for null entries which
    // increases
    // memory usage but allows for O(1) set operations.
    kDense,
  };

 public:
  // Creates an empty NullableVector.
  NullableVector() : NullableVector<T>(Mode::kSparse) {}

  NullableVector(const NullableVector&) = delete;
  NullableVector& operator=(const NullableVector&) = delete;

  NullableVector(NullableVector&&) = default;
  NullableVector& operator=(NullableVector&&) noexcept = default;

  // Creates a sparse nullable vector
  static NullableVector<T> Sparse() { return NullableVector<T>(Mode::kSparse); }

  // Creates a dense nullable vector
  static NullableVector<T> Dense() { return NullableVector<T>(Mode::kDense); }

  // Returns the optional value at |idx| or std::nullopt if the value is null.
  std::optional<T> Get(uint32_t idx) const {
    bool contains = valid_.IsSet(idx);
    if (mode_ == Mode::kDense)
      return contains ? std::make_optional(data_[idx]) : std::nullopt;

    return contains ? std::make_optional(data_[valid_.CountSetBits(idx)])
                    : std::nullopt;
  }

  // Adds the given value to the NullableVector.
  void Append(T val) {
    data_.emplace_back(val);
    valid_.AppendTrue();
  }

  // Adds the given optional value to the NullableVector.
  void Append(std::optional<T> val) {
    if (val) {
      Append(*val);
    } else {
      AppendNull();
    }
  }

  // Sets the value at |idx| to the given |val|.
  void Set(uint32_t idx, T val) {
    if (mode_ == Mode::kDense) {
      valid_.Set(idx);
      data_[idx] = val;
    } else {
      // Generally, we will be setting a null row to non-null so optimize for
      // that path.
      uint32_t row = valid_.CountSetBits(idx);
      bool was_set = valid_.Set(idx);
      if (PERFETTO_UNLIKELY(was_set)) {
        data_[row] = val;
      } else {
        data_.insert(data_.begin() + static_cast<ptrdiff_t>(row), val);
      }
    }
  }

  // Requests the removal of unused capacity.
  // Matches the semantics of std::vector::shrink_to_fit.
  void ShrinkToFit() {
    data_.shrink_to_fit();
    valid_.ShrinkToFit();
  }

  // Returns the size of the NullableVector; this includes any null values.
  uint32_t size() const { return valid_.size(); }

  // Returns whether data in this NullableVector is stored densely.
  bool IsDense() const { return mode_ == Mode::kDense; }

  const std::vector<T>& non_null_vector() const { return data_; }
  const BitVector& non_null_bit_vector() const { return valid_; }

 private:
  explicit NullableVector(Mode mode) : mode_(mode) {}

  void AppendNull() {
    if (mode_ == Mode::kDense) {
      data_.emplace_back();
    }
    valid_.AppendFalse();
  }

  Mode mode_ = Mode::kSparse;

  std::vector<T> data_;
  BitVector valid_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_NULLABLE_VECTOR_H_
