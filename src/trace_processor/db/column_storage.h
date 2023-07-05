/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_H_

#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/nullable_vector.h"

namespace perfetto {
namespace trace_processor {

// Base class for allowing type erasure when defining plug-in implementations
// of backing storage for columns.
class ColumnStorageBase {
 public:
  ColumnStorageBase() = default;
  virtual ~ColumnStorageBase();

  ColumnStorageBase(const ColumnStorageBase&) = delete;
  ColumnStorageBase& operator=(const ColumnStorageBase&) = delete;

  ColumnStorageBase(ColumnStorageBase&&) = default;
  ColumnStorageBase& operator=(ColumnStorageBase&&) noexcept = default;

  virtual const void* data() const = 0;
  virtual const BitVector* bv() const = 0;
  virtual uint32_t size() const = 0;
  virtual uint32_t non_null_size() const = 0;
};

// Class used for implementing storage for non-null columns.
template <typename T>
class ColumnStorage final : public ColumnStorageBase {
 public:
  ColumnStorage() = default;

  explicit ColumnStorage(const ColumnStorage&) = delete;
  ColumnStorage& operator=(const ColumnStorage&) = delete;

  ColumnStorage(ColumnStorage&&) = default;
  ColumnStorage& operator=(ColumnStorage&&) noexcept = default;

  T Get(uint32_t idx) const { return vector_[idx]; }
  void Append(T val) { vector_.emplace_back(val); }
  void Set(uint32_t idx, T val) { vector_[idx] = val; }
  void ShrinkToFit() { vector_.shrink_to_fit(); }
  const std::vector<T>& vector() const { return vector_; }

  const void* data() const final { return vector_.data(); }
  const BitVector* bv() const final { return nullptr; }
  uint32_t size() const final { return static_cast<uint32_t>(vector_.size()); }
  uint32_t non_null_size() const final { return size(); }

  template <bool IsDense>
  static ColumnStorage<T> Create() {
    static_assert(!IsDense, "Invalid for non-null storage to be dense.");
    return ColumnStorage<T>();
  }

 private:
  std::vector<T> vector_;
};

// Class used for implementing storage for nullable columns.
template <typename T>
class ColumnStorage<std::optional<T>> final : public ColumnStorageBase {
 public:
  ColumnStorage() = default;

  explicit ColumnStorage(const ColumnStorage&) = delete;
  ColumnStorage& operator=(const ColumnStorage&) = delete;

  ColumnStorage(ColumnStorage&&) = default;
  ColumnStorage& operator=(ColumnStorage&&) noexcept = default;

  std::optional<T> Get(uint32_t idx) const { return nv_.Get(idx); }
  void Append(T val) { nv_.Append(val); }
  void Append(std::optional<T> val) { nv_.Append(std::move(val)); }
  void Set(uint32_t idx, T val) { nv_.Set(idx, val); }
  bool IsDense() const { return nv_.IsDense(); }
  void ShrinkToFit() { nv_.ShrinkToFit(); }
  // For dense columns the size of the vector is equal to size of the bit
  // vector. For sparse it's equal to count set bits of the bit vector.
  const std::vector<T>& non_null_vector() const {
    return nv_.non_null_vector();
  }
  const BitVector& non_null_bit_vector() const {
    return nv_.non_null_bit_vector();
  }

  const void* data() const final { return nv_.non_null_vector().data(); }
  const BitVector* bv() const final { return &nv_.non_null_bit_vector(); }
  uint32_t size() const final { return nv_.size(); }
  uint32_t non_null_size() const final {
    return static_cast<uint32_t>(nv_.non_null_vector().size());
  }

  template <bool IsDense>
  static ColumnStorage<std::optional<T>> Create() {
    return IsDense
               ? ColumnStorage<std::optional<T>>(NullableVector<T>::Dense())
               : ColumnStorage<std::optional<T>>(NullableVector<T>::Sparse());
  }

 private:
  explicit ColumnStorage(NullableVector<T> nv) : nv_(std::move(nv)) {}

  NullableVector<T> nv_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_STORAGE_H_
