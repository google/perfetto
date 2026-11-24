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

#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_ARENA_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"

namespace perfetto::trace_processor::core {

// A run of `size()` Ts in an Arena. Copying a span copies the pointer, not the
// Ts. It needs no destructor because the arena frees the memory.
template <typename T>
class ArenaSpan {
 public:
  ArenaSpan() = default;
  ArenaSpan(T* data, uint32_t size) : data_(data), size_(size) {}

  T* begin() const { return data_; }
  T* end() const { return data_ + size_; }
  uint32_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  T& operator[](uint32_t i) const {
    PERFETTO_DCHECK(i < size_);
    return data_[i];
  }
  T& back() const {
    PERFETTO_DCHECK(size_ > 0);
    return data_[size_ - 1];
  }

  base::Span<const T> span() const { return {data_, data_ + size_}; }
  operator base::Span<const T>() const { return span(); }

  // Keeps the first `size`.
  void Shrink(uint32_t size) {
    PERFETTO_DCHECK(size <= size_);
    size_ = size;
  }

  // Compacts the span in place, dropping every element where `pred` returns
  // true.
  template <typename Pred>
  void RemoveIf(Pred pred) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < size_; ++i) {
      if (!pred(data_[i])) {
        if (kept != i) {
          data_[kept] = data_[i];
        }
        ++kept;
      }
    }
    size_ = kept;
  }

 private:
  T* data_ = nullptr;
  uint32_t size_ = 0;
};

// Memory that is freed all at once, when the arena is destroyed. Nothing made
// in an arena is ever destroyed, so it must be trivially destructible: plain
// values, pointers to things that outlive the arena, and ArenaSpans. Such types
// need no destructor code, which is the point: freeing a plan costs nothing.
class Arena {
 public:
  Arena() = default;
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&& other) noexcept
      : next_(std::exchange(other.next_, 0)),
        end_(std::exchange(other.end_, 0)),
        blocks_(std::exchange(other.blocks_, nullptr)) {}
  Arena& operator=(Arena&& other) noexcept {
    if (this != &other) {
      FreeBlocks();
      next_ = std::exchange(other.next_, 0);
      end_ = std::exchange(other.end_, 0);
      blocks_ = std::exchange(other.blocks_, nullptr);
    }
    return *this;
  }

  template <typename T, typename... Args>
  T* New(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "an arena never destroys what it holds");
    return new (Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  // Allocates `size` uninitialized Ts. The caller must set each one before
  // reading it.
  template <typename T>
  ArenaSpan<T> NewSpan(uint32_t size) {
    static_assert(
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
        "an arena never destroys what it holds");
    if (size == 0) {
      return {};
    }
    return {static_cast<T*>(Allocate(sizeof(T) * size, alignof(T))), size};
  }

  // A copy of `values`.
  template <typename T>
  ArenaSpan<T> CopySpan(base::Span<const T> values) {
    ArenaSpan<T> span = NewSpan<T>(static_cast<uint32_t>(values.size()));
    if (!values.empty()) {
      memcpy(span.begin(), values.data(), values.size() * sizeof(T));
    }
    return span;
  }

  // Allocates `size` bytes aligned to `align`. Kept out of line, so each
  // allocation is a call rather than inlined code.
  void* Allocate(size_t size, size_t align);

 private:
  struct Block {
    Block* prev;
  };

  void FreeBlocks();

  // Starts a block holding at least `size` bytes aligned to `align`.
  void* AllocateBlock(size_t size, size_t align);

  uintptr_t next_ = 0;
  uintptr_t end_ = 0;
  Block* blocks_ = nullptr;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_ARENA_H_
