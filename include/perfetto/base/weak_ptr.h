/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_BASE_WEAK_PTR_H_
#define INCLUDE_PERFETTO_BASE_WEAK_PTR_H_

#include "perfetto/base/thread_checker.h"

#include <memory>

namespace perfetto {
namespace base {

// A simple WeakPtr for single-threaded cases.
// Usage:
// class MyClass {
//  MyClass() : weak_factory_(this) {}
//  WeakPtr<MyClass> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
//
// private:
//  WeakPtrFactory<MyClass> weak_factory_;
// }
//
// int main() {
//  std::unique_ptr<MyClass> foo(new MyClass);
//  auto wptr = foo.GetWeakPtr();
//  ASSERT_TRUE(wptr);
//  ASSERT_EQ(foo.get(), wptr->get());
//  foo.reset();
//  ASSERT_FALSE(wptr);
//  ASSERT_EQ(nullptr, wptr->get());
// }

template <typename T>
class WeakPtrFactory;  // Forward declaration, defined below.

template <typename T>
class WeakPtr {
 public:
  WeakPtr() {}
  WeakPtr(const WeakPtr&) = default;
  WeakPtr& operator=(const WeakPtr&) = default;
  WeakPtr(WeakPtr&&) = default;
  WeakPtr& operator=(WeakPtr&&) = default;

  T* get() const {
    PERFETTO_DCHECK_THREAD(thread_checker);
    return handle_ ? *handle_.get() : nullptr;
  }
  T* operator->() const { return get(); }
  T& operator*() const { return *get(); }

  explicit operator bool() const { return !!get(); }

 private:
  friend class WeakPtrFactory<T>;
  explicit WeakPtr(const std::shared_ptr<T*>& handle) : handle_(handle) {}

  std::shared_ptr<T*> handle_;
  PERFETTO_THREAD_CHECKER(thread_checker)
};

template <typename T>
class WeakPtrFactory {
 public:
  explicit WeakPtrFactory(T* owner) : handle_(new T* {owner}) {
    PERFETTO_DCHECK_THREAD(thread_checker);
  }
  ~WeakPtrFactory() {
    PERFETTO_DCHECK_THREAD(thread_checker);
    *(handle_.get()) = nullptr;
  }

  WeakPtr<T> GetWeakPtr() const {
    PERFETTO_DCHECK_THREAD(thread_checker);
    return WeakPtr<T>(handle_);
  }

 private:
  WeakPtrFactory(const WeakPtrFactory&) = delete;
  WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;

  std::shared_ptr<T*> handle_;
  PERFETTO_THREAD_CHECKER(thread_checker)
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_WEAK_PTR_H_
