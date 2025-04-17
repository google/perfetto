/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_STATIC_VECTOR_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_STATIC_VECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::dataframe::impl {

// A hybrid class between std::vector and std::array.
//
// This class has a fixed inline memory for storing elements (similar to
// std::array) but has a variable size.
//
// Name is inspired by boost::container:fixed_vector.
template <typename T, size_t kCapacity>
class FixedVector {
 public:
  static_assert(kCapacity > 0, "Cannot have zero capcity FixedVector");

  constexpr FixedVector() = default;

  // Only available if T is default constructible.
  template <typename U = T,
            typename = std::enable_if_t<std::is_default_constructible_v<U>>>
  explicit FixedVector(std::size_t count) {
    PERFETTO_DCHECK(count <= kCapacity);
    for (std::size_t i = 0; i < count; ++i) {
      new (get_ptr(i)) T();
    }
    size_ = count;
  }

  ~FixedVector() { destroy_all(); }

  FixedVector(const FixedVector& other) {
    for (size_t i = 0; i < other.size_; ++i) {
      new (get_ptr(i)) T(other[i]);
    }
    size_ = other.size_;
  }
  FixedVector(FixedVector&& other) {
    for (size_t i = 0; i < other.size_; ++i) {
      new (get_ptr(i)) T(std::move(other[i]));
    }
    size_ = other.size_;
    other.size_ = 0;
  }

  FixedVector& operator=(const FixedVector& other) {
    if (this != &other) {
      destroy_all();
      for (size_t i = 0; i < other.size_; ++i) {
        new (get_ptr(i)) T(other[i]);
      }
      size_ = other.size_;
    }
    return *this;
  }
  FixedVector& operator=(FixedVector&& other) {
    if (this != &other) {
      destroy_all();
      for (size_t i = 0; i < other.size_; ++i) {
        new (get_ptr(i)) T(std::move(other[i]));
      }
      size_ = other.size_;
      other.size_ = 0;
    }
    return *this;
  }

  constexpr size_t size() const { return size_; }

  T* data() { return get_ptr(0); }
  const T* data() const { return get_ptr(0); }

  T* begin() { return data(); }
  const T* begin() const { return data(); }

  T* end() { return data() + size_; }
  const T* end() const { return data() + size_; }

  T& operator[](size_t pos) { return *get_ptr(pos); }
  const T& operator[](size_t pos) const { return *get_ptr(pos); }

  void push_back(const T& value) {
    PERFETTO_DCHECK(size_ < kCapacity);
    new (get_ptr(size_)) T(value);
    ++size_;
  }

  void push_back(T&& value) {
    PERFETTO_DCHECK(size_ < kCapacity);
    new (get_ptr(size_)) T(std::move(value));
    ++size_;
  }

  template <typename... Args>
  T& emplace_back(Args&&... args) {
    PERFETTO_DCHECK(size_ < kCapacity);
    T* constructed_ptr = new (get_ptr(size_)) T(std::forward<Args>(args)...);
    ++size_;
    return *constructed_ptr;
  }

 private:
  void destroy_all() {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      std::destroy(begin(), end());
    }
  }

  T* get_ptr(size_t index) {
    return std::launder(reinterpret_cast<T*>(storage_ + (index * sizeof(T))));
  }

  const T* get_ptr(size_t index) const {
    return std::launder(
        reinterpret_cast<const T*>(storage_ + (index * sizeof(T))));
  }

  alignas(T) char storage_[sizeof(T) * kCapacity];
  size_t size_ = 0;
};

}  // namespace perfetto::trace_processor::dataframe::impl

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_STATIC_VECTOR_H_
