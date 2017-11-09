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

#ifndef PERFETTO_BASE_LOGGING_H_
#define PERFETTO_BASE_LOGGING_H_

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(NDEBUG)
#define PERFETTO_DCHECK_IS_ON() 0
#else
#define PERFETTO_DCHECK_IS_ON() 1
#include <stdio.h>   // For fprintf.
#include <string.h>  // For strerror.
#endif

#include "base/utils.h"

#if PERFETTO_DCHECK_IS_ON()
#define PERFETTO_DLOG(fmt, ...)                                               \
  fprintf(stderr, "\n[%s:%d, errno: %d %s]\n" fmt "\n\n", __FILE__, __LINE__, \
          errno, errno ? strerror(errno) : "", ##__VA_ARGS__)
#define PERFETTO_DPLOG(...) PERFETTO_DLOG(__VA_ARGS__)
#define PERFETTO_DCHECK(x)                            \
  do {                                                \
    if (!__builtin_expect(!!(x), true)) {             \
      PERFETTO_DPLOG("%s", "PERFETTO_CHECK(" #x ")"); \
      abort();                                        \
    }                                                 \
  } while (0)
#else
#define PERFETTO_DLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#define PERFETTO_DPLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#define PERFETTO_DCHECK(x) ::perfetto::base::ignore_result(x)
#endif  // PERFETTO_DCHECK_IS_ON()

#if PERFETTO_DCHECK_IS_ON()
#define PERFETTO_CHECK(x) PERFETTO_DCHECK(x)
#else
#define PERFETTO_CHECK(x)               \
  do {                                  \
    if (!__builtin_expect(!!(x), true)) \
      abort();                          \
  } while (0)
#endif  // PERFETTO_DCHECK_IS_ON()

#endif  // PERFETTO_BASE_LOGGING_H_
