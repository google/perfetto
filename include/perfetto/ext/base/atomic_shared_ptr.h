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

#ifndef INCLUDE_PERFETTO_EXT_BASE_ATOMIC_SHARED_PTR_H_
#define INCLUDE_PERFETTO_EXT_BASE_ATOMIC_SHARED_PTR_H_

#include <atomic>
#include <memory>

namespace perfetto {
namespace base {

// Wrapper providing a uniform API for atomic shared_ptr operations across
// pre-C++20 (free functions) and C++20+ (std::atomic<std::shared_ptr<T>>).
template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() noexcept = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> p) noexcept {
    store(std::move(p));
  }

  // Non-copyable (like many atomic wrappers). Movable for convenience.
  AtomicSharedPtr(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
  AtomicSharedPtr(AtomicSharedPtr&&) = delete;
  AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

  std::shared_ptr<T> load(
      std::memory_order order = std::memory_order_seq_cst) const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
    return ptr_.load(order);
#else
    return std::atomic_load_explicit(&ptr_, order);
#endif
  }

  void store(std::shared_ptr<T> desired,
             std::memory_order order = std::memory_order_seq_cst) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
    ptr_.store(std::move(desired), order);
#else
    std::atomic_store_explicit(&ptr_, std::move(desired), order);
#endif
  }

  bool compare_exchange_strong(
      std::shared_ptr<T>& expected,
      std::shared_ptr<T> desired,
      std::memory_order success = std::memory_order_seq_cst,
      std::memory_order failure = std::memory_order_seq_cst) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr)
    return ptr_.compare_exchange_strong(expected, std::move(desired), success,
                                        failure);
#else
    return std::atomic_compare_exchange_strong_explicit(
        &ptr_, &expected, std::move(desired), success, failure);
#endif
  }

 private:
#if defined(__cpp_lib_atomic_shared_ptr)  // C++20 and later
  std::atomic<std::shared_ptr<T>> ptr_{};
#else  // pre-C++20: use free-function atomics with a plain shared_ptr
  mutable std::shared_ptr<T> ptr_{};
#endif
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_ATOMIC_SHARED_PTR_H_
