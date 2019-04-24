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

#ifndef INCLUDE_PERFETTO_BASE_NO_DESTRUCTOR_H_
#define INCLUDE_PERFETTO_BASE_NO_DESTRUCTOR_H_

#include <new>
#include <utility>

namespace perfetto {
namespace base {

// Wrapper that can hold an object of type T, without invoking the contained
// object's destructor when being destroyed. Useful for creating statics while
// avoiding static destructors.
//
// Stores the object inline, and therefore doesn't incur memory allocation and
// pointer indirection overheads.
//
// Example of use:
//
//   const std::string& GetStr() {
//     static base::NoDestructor<std::string> s("hello");
//     return s.ref();
//   }
//
template <typename T>
class NoDestructor {
 public:
  // Forward arguments to T's constructor. Note that this doesn't cover
  // construction from initializer lists.
  template <typename... Args>
  explicit NoDestructor(Args&&... args) {
    new (storage_) T(std::forward<Args>(args)...);
  }

  NoDestructor(const NoDestructor&) = delete;
  NoDestructor& operator=(const NoDestructor&) = delete;
  NoDestructor(NoDestructor&&) = delete;
  NoDestructor& operator=(NoDestructor&&) = delete;

  ~NoDestructor() = default;

  const T& ref() const { return *reinterpret_cast<const T*>(storage_); }
  T& ref() { return *reinterpret_cast<T*>(storage_); }

 private:
  alignas(T) char storage_[sizeof(T)];
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_NO_DESTRUCTOR_H_
