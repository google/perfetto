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

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/containers/row_map.h"

namespace perfetto {
namespace trace_processor {

// Base class for NullableVector which allows type erasure to be implemented
// (e.g. allows for std::unique_ptr<NullableVectorBase>).
class NullableVectorBase {
 public:
  NullableVectorBase() = default;
  virtual ~NullableVectorBase();

  NullableVectorBase(const NullableVectorBase&) = delete;
  NullableVectorBase& operator=(const NullableVectorBase&) = delete;

  NullableVectorBase(NullableVectorBase&&) = default;
  NullableVectorBase& operator=(NullableVectorBase&&) noexcept = default;
};

// A data structure which compactly stores a list of possibly nullable data.
//
// Internally, this class is implemented using a combination of a std::deque
// with a BitVector used to store whether each index is null or not.
// By default, for each null value, it only uses a single bit inside the
// BitVector at a slight cost (searching the BitVector to find the index into
// the std::deque) when looking up the data.
template <typename T>
class NullableVector : public NullableVectorBase {
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
  ~NullableVector() override = default;

  explicit NullableVector(const NullableVector&) = delete;
  NullableVector& operator=(const NullableVector&) = delete;

  NullableVector(NullableVector&&) = default;
  NullableVector& operator=(NullableVector&&) noexcept = default;

  // Creates a sparse nullable vector
  static NullableVector<T> Sparse() { return NullableVector<T>(Mode::kSparse); }

  // Creates a dense nullable vector
  static NullableVector<T> Dense() { return NullableVector<T>(Mode::kDense); }

  // Returns the optional value at |idx| or base::nullopt if the value is null.
  base::Optional<T> Get(uint32_t idx) const {
    if (mode_ == Mode::kDense) {
      bool contains = valid_.Contains(idx);
      return contains ? base::Optional<T>(data_[idx]) : base::nullopt;
    } else {
      auto opt_idx = valid_.IndexOf(idx);
      return opt_idx ? base::Optional<T>(data_[*opt_idx]) : base::nullopt;
    }
  }

  // Returns the non-null value at |ordinal| where |ordinal| gives the index
  // of the entry in-terms of non-null entries only.
  //
  // For example:
  // this = [0, null, 2, null, 4]
  //
  // GetNonNull(0) = 0
  // GetNonNull(1) = 2
  // GetNonNull(2) = 4
  // ...
  T GetNonNull(uint32_t ordinal) const {
    if (mode_ == Mode::kDense) {
      return data_[valid_.Get(ordinal)];
    } else {
      PERFETTO_DCHECK(ordinal < data_.size());
      return data_[ordinal];
    }
  }

  // Adds the given value to the NullableVector.
  void Append(T val) {
    data_.emplace_back(val);
    valid_.Insert(size_++);
  }

  // Adds a null value to the NullableVector.
  void AppendNull() {
    if (mode_ == Mode::kDense) {
      data_.emplace_back();
    }
    size_++;
  }

  // Adds the given optional value to the NullableVector.
  void Append(base::Optional<T> val) {
    if (val) {
      Append(*val);
    } else {
      AppendNull();
    }
  }

  // Sets the value at |idx| to the given |val|.
  void Set(uint32_t idx, T val) {
    if (mode_ == Mode::kDense) {
      if (!valid_.Contains(idx)) {
        valid_.Insert(idx);
      }
      data_[idx] = val;
    } else {
      auto opt_idx = valid_.IndexOf(idx);

      // Generally, we will be setting a null row to non-null so optimize for
      // that path.
      if (PERFETTO_UNLIKELY(opt_idx)) {
        data_[*opt_idx] = val;
      } else {
        valid_.Insert(idx);

        opt_idx = valid_.IndexOf(idx);
        PERFETTO_DCHECK(opt_idx);
        data_.insert(data_.begin() + static_cast<ptrdiff_t>(*opt_idx), val);
      }
    }
  }

  // Returns the size of the NullableVector; this includes any null values.
  uint32_t size() const { return size_; }

  // Returns whether data in this NullableVector is stored densely.
  bool IsDense() const { return mode_ == Mode::kDense; }

 private:
  NullableVector(Mode mode) : mode_(mode) {}

  Mode mode_ = Mode::kSparse;

  std::deque<T> data_;
  RowMap valid_;
  uint32_t size_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_NULLABLE_VECTOR_H_
