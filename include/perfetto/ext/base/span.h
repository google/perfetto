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

#ifndef INCLUDE_PERFETTO_EXT_BASE_SPAN_H_
#define INCLUDE_PERFETTO_EXT_BASE_SPAN_H_

#include <cstddef>
#include <vector>

namespace perfetto::base {

// Minimal non-owning view over contiguous data. Compatible with range-based
// for loops and forward-compatible with std::span (C++20).
template <typename T>
struct Span {
  // STL-style typedefs for compatibility with algorithms and gmock matchers.
  using value_type = T;
  using const_iterator = const T*;
  using iterator = const T*;

  constexpr Span() = default;
  constexpr Span(const T* d, size_t s) : data_(d), size_(s) {}

  constexpr const T* data() const { return data_; }
  constexpr size_t size() const { return size_; }
  constexpr const T* begin() const { return data_; }
  constexpr const T* end() const { return data_ + size_; }
  constexpr bool empty() const { return size_ == 0; }
  constexpr const T& operator[](size_t i) const { return data_[i]; }

 private:
  const T* data_ = nullptr;
  size_t size_ = 0;
};

// Deduction guides
template <typename T>
Span(const T*, size_t) -> Span<T>;

template <typename T>
Span(const std::vector<T>&) -> Span<T>;

// Helper to create a Span from a vector (always const since vector is const
// ref)
template <typename T>
constexpr Span<const T> MakeSpan(const std::vector<T>& vec) {
  return Span<const T>{vec.data(), vec.size()};
}

// Helper to create a Span from pointer and size
template <typename T>
constexpr Span<T> MakeSpan(const T* data, size_t size) {
  return Span<T>{data, size};
}

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_SPAN_H_
