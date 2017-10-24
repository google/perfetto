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

#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
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
#define DPLOG(x)                                                          \
  fprintf(stderr, x " %s:%d (errno: %d %s)\n", __FILE__, __LINE__, errno, \
          strerror(errno))
#define DCHECK(x)      \
  do {                 \
    if (!(x)) {        \
      DPLOG("CHECK "); \
      abort();         \
    }                  \
  } while (0)
#else
#define DLOG(...) \
  do {            \
  } while (0)
#define DPLOG(...) \
  do {             \
  } while (0)
#define DCHECK(x) ::ignore_result(x)
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

template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];
#define arraysize(array) (sizeof(ArraySizeHelper(array)))

template <typename T>
inline void ignore_result(const T&) {}

#endif  // CPP_COMMON_BASE_H_
