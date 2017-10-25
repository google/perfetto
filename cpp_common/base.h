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

#ifndef CPP_COMMON_BASE_H_
#define CPP_COMMON_BASE_H_

// DO NOT include this file in public headers (include/) to avoid collisions.

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(NDEBUG)
#define DCHECK_IS_ON() 0
#else
#define DCHECK_IS_ON() 1
#endif

#if DCHECK_IS_ON()
#include <stdio.h>   // For fprintf.
#include <string.h>  // For strerror.
#endif

#define HANDLE_EINTR(x)                                     \
  ({                                                        \
    decltype(x) eintr_wrapper_result;                       \
    do {                                                    \
      eintr_wrapper_result = (x);                           \
    } while (eintr_wrapper_result == -1 && errno == EINTR); \
    eintr_wrapper_result;                                   \
  })

#if DCHECK_IS_ON()
#define DLOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define DPLOG(x)                                                    \
  DLOG("%s %s:%d (errno: %d %s)\n", (x), __FILE__, __LINE__, errno, \
       strerror(errno))
#define DCHECK(x)                         \
  do {                                    \
    if (!__builtin_expect(!!(x), true)) { \
      DPLOG("CHECK(" #x ")");             \
      abort();                            \
    }                                     \
  } while (0)
#else
#define DLOG(...) ::perfetto::ignore_result(__VA_ARGS__)
#define DPLOG(...) ::perfetto::ignore_result(__VA_ARGS__)
#define DCHECK(x) ::perfetto::ignore_result(x)
#endif  // DCHECK_IS_ON()

#if DCHECK_IS_ON()
#define CHECK(x) DCHECK(x)
#else
#define CHECK(x)                        \
  do {                                  \
    if (!__builtin_expect(!!(x), true)) \
      abort();                          \
  } while (0)
#endif  // DCHECK_IS_ON()

namespace perfetto {

template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];
#define arraysize(array) (sizeof(::perfetto::ArraySizeHelper(array)))

template <typename... T>
inline void ignore_result(const T&...) {}

// RAII classes for auto-releasing fd/dirs.
template <typename T, int (*CloseFunction)(T), T InvalidValue>
class ScopedResource {
 public:
  explicit ScopedResource(T t = InvalidValue) : t_(t) {}
  ScopedResource(ScopedResource&& other) noexcept {
    t_ = other.t_;
    other.t_ = InvalidValue;
  }
  ScopedResource& operator=(ScopedResource&& other) {
    reset(other.t_);
    other.t_ = InvalidValue;
    return *this;
  }
  T get() const { return t_; }
  void reset(T r = InvalidValue) {
    if (t_ != InvalidValue) {
      int res = CloseFunction(t_);
      CHECK(res == 0);
    }
    t_ = r;
  }
  ~ScopedResource() { reset(InvalidValue); }

 private:
  ScopedResource(const ScopedResource&) = delete;
  ScopedResource& operator=(const ScopedResource&) = delete;

  T t_;
};

using ScopedFile = ScopedResource<int, close, -1>;

}  // namespace perfetto

#endif  // CPP_COMMON_BASE_H_
