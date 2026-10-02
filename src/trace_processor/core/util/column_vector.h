/*
 * Copyright (C) 2026 The Android Open Source Project
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
#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_COLUMN_VECTOR_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_COLUMN_VECTOR_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core {

struct ColumnVectorBackingState {
  void* data;
  uint64_t capacity;
};

class ColumnVectorBacking {
 public:
  virtual ~ColumnVectorBacking();
  virtual ColumnVectorBackingState state() const = 0;
  virtual ColumnVectorBackingState Resize(uint64_t size,
                                          uint64_t capacity) = 0;
  virtual ColumnVectorBackingState ShrinkToFit(uint64_t size) = 0;
};

uint64_t ComputeColumnVectorCapacity(uint64_t current, uint64_t requested);

// A FlexVector-compatible container which can also retain external contiguous
// storage. The active backing is only inspected on cold operations. Append and
// element access use the cached pointer, size and capacity directly.
template <typename T>
class ColumnVector {
 private:
  class FlexVectorBacking final : public ColumnVectorBacking {
   public:
    explicit FlexVectorBacking(FlexVector<T> values)
        : values_(std::move(values)) {}

    ColumnVectorBackingState state() const override {
      return {const_cast<T*>(values_.data()), values_.capacity()};
    }
    ColumnVectorBackingState Resize(uint64_t size,
                                    uint64_t capacity) override {
      values_.resize(size);
      values_.reserve(capacity);
      return state();
    }
    ColumnVectorBackingState ShrinkToFit(uint64_t size) override {
      values_.resize(size);
      values_.shrink_to_fit();
      return state();
    }

   private:
    FlexVector<T> values_;
  };

 public:
  static constexpr uint64_t kCapacityMultiple =
      FlexVector<T>::kCapacityMultiple;

  ColumnVector() : ColumnVector(FlexVector<T>()) {}
  // Allows existing dataframe builders to hand off their FlexVector without
  // copying. Subsequent cold operations continue through that same vector.
  ColumnVector(FlexVector<T> owned)
      : data_(owned.data()),
        size_(owned.size()),
        capacity_(owned.capacity()),
        backing_(std::make_unique<FlexVectorBacking>(std::move(owned))) {}
  ColumnVector(ColumnVector&&) noexcept = default;
  ColumnVector& operator=(ColumnVector&&) noexcept = default;
  ColumnVector(const ColumnVector&) = delete;
  ColumnVector& operator=(const ColumnVector&) = delete;

  static ColumnVector CreateWithCapacity(uint64_t capacity) {
    return ColumnVector(FlexVector<T>::CreateWithCapacity(capacity), 0);
  }
  static ColumnVector CreateWithSize(uint64_t size) {
    return ColumnVector(FlexVector<T>::CreateWithSize(size), size);
  }
  static ColumnVector CreateFilled(uint64_t size, T value) {
    return ColumnVector(FlexVector<T>::CreateFilled(size, value), size);
  }
  static ColumnVector AdoptBacking(
      uint64_t size,
      std::unique_ptr<ColumnVectorBacking> backing,
      bool writable) {
    PERFETTO_CHECK(backing);
    ColumnVectorBackingState state = backing->state();
    PERFETTO_CHECK(size <= state.capacity);
    return ColumnVector(state, size, std::move(backing), writable);
  }

  PERFETTO_ALWAYS_INLINE void push_back(T value) {
    PERFETTO_DCHECK(writable_);
    if (PERFETTO_UNLIKELY(size_ == capacity_))
      Grow();
    data_[size_++] = value;
  }
  PERFETTO_ALWAYS_INLINE void push_back_multiple(T value, uint64_t count) {
    reserve(size_ + count);
    std::fill_n(data_ + size_, count, value);
    size_ += count;
  }
  PERFETTO_ALWAYS_INLINE void pop_back() { --size_; }
  void clear() { size_ = 0; }
  void reserve(uint64_t requested) {
    PERFETTO_DCHECK(writable_);
    if (requested <= capacity_)
      return;
    Reallocate(ComputeColumnVectorCapacity(capacity_, requested));
  }
  void resize(uint64_t size) {
    reserve(size);
    size_ = size;
  }
  void shrink_to_fit() {
    if (!writable_)
      return;
    Refresh(backing_->ShrinkToFit(size_));
  }

  PERFETTO_ALWAYS_INLINE T* data() {
    PERFETTO_DCHECK(writable_);
    return data_;
  }
  PERFETTO_ALWAYS_INLINE const T* data() const { return data_; }
  PERFETTO_ALWAYS_INLINE uint64_t size() const { return size_; }
  PERFETTO_ALWAYS_INLINE uint64_t capacity() const { return capacity_; }
  PERFETTO_ALWAYS_INLINE bool empty() const { return size_ == 0; }
  PERFETTO_ALWAYS_INLINE Span<T> mutable_span() {
    return Span<T>(data(), data() + size_);
  }
  PERFETTO_ALWAYS_INLINE Span<const T> span() const {
    return Span<const T>(data_, data_ + size_);
  }
  PERFETTO_ALWAYS_INLINE T* begin() { return data(); }
  PERFETTO_ALWAYS_INLINE T* end() { return data() + size_; }
  PERFETTO_ALWAYS_INLINE const T* begin() const { return data_; }
  PERFETTO_ALWAYS_INLINE const T* end() const { return data_ + size_; }
  PERFETTO_ALWAYS_INLINE T& operator[](uint64_t i) {
    PERFETTO_DCHECK(writable_ && i < size_);
    return data_[i];
  }
  PERFETTO_ALWAYS_INLINE const T& operator[](uint64_t i) const {
    PERFETTO_DCHECK(i < size_);
    return data_[i];
  }
  PERFETTO_ALWAYS_INLINE T& back() { return (*this)[size_ - 1]; }
  PERFETTO_ALWAYS_INLINE const T& back() const { return (*this)[size_ - 1]; }

 private:
  ColumnVector(FlexVector<T> owned, uint64_t size)
      : data_(owned.data()),
        size_(size),
        capacity_(owned.capacity()),
        backing_(std::make_unique<FlexVectorBacking>(std::move(owned))) {}
  ColumnVector(ColumnVectorBackingState state,
               uint64_t size,
               std::unique_ptr<ColumnVectorBacking> backing,
               bool writable)
      : data_(static_cast<T*>(state.data)),
        size_(size),
        capacity_(state.capacity),
        backing_(std::move(backing)),
        writable_(writable) {}

  PERFETTO_NO_INLINE void Grow() {
    Reallocate(ComputeColumnVectorCapacity(capacity_, capacity_ + 1));
  }
  PERFETTO_NO_INLINE void Reallocate(uint64_t capacity) {
    Refresh(backing_->Resize(size_, capacity));
  }
  void Refresh(ColumnVectorBackingState state) {
    PERFETTO_CHECK(state.data || state.capacity == 0);
    data_ = static_cast<T*>(state.data);
    capacity_ = state.capacity;
  }

  T* data_ = nullptr;
  uint64_t size_ = 0;
  uint64_t capacity_ = 0;
  std::unique_ptr<ColumnVectorBacking> backing_;
  bool writable_ = true;
};

}  // namespace perfetto::trace_processor::core
#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_COLUMN_VECTOR_H_
