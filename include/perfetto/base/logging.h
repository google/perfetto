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

#ifndef INCLUDE_PERFETTO_BASE_LOGGING_H_
#define INCLUDE_PERFETTO_BASE_LOGGING_H_

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(NDEBUG)
#define PERFETTO_DCHECK_IS_ON() 0
#else
#define PERFETTO_DCHECK_IS_ON() 1
#include <string.h>  // For strerror.
#endif

#include "perfetto/base/utils.h"

namespace perfetto {
namespace base {

// Constexpr functions to extract basename(__FILE__), e.g.: ../foo/f.c -> f.c .
constexpr const char* StrEnd(const char* s) {
  return *s ? StrEnd(s + 1) : s;
}

constexpr const char* BasenameRecursive(const char* s,
                                        const char* begin,
                                        const char* end) {
  return (*s == '/' && s < end)
             ? (s + 1)
             : ((s > begin) ? BasenameRecursive(s - 1, begin, end) : s);
}

constexpr const char* Basename(const char* str) {
  return BasenameRecursive(StrEnd(str), str, StrEnd(str));
}

enum LogLev { kLogDebug = 0, kLogInfo, kLogImportant, kLogError };
constexpr const char* kLogFmt[] = {"\x1b[2m", "\x1b[39m", "\x1b[32m\x1b[1m",
                                   "\x1b[31m"};

#define PERFETTO_LOG_LINE__(x) #x
#define PERFETTO_LOG_LINE_(x) PERFETTO_LOG_LINE__(x)
#define PERFETTO_LOG_LINE PERFETTO_LOG_LINE_(__LINE__)

#define PERFETTO_XLOG(level, fmt, ...)                                \
  fprintf(stderr, "\x1b[90m%-24.24s\x1b[0m %s" fmt "\x1b[0m\n",       \
          ::perfetto::base::Basename(__FILE__ ":" PERFETTO_LOG_LINE), \
          ::perfetto::base::kLogFmt[::perfetto::base::LogLev::level], \
          ##__VA_ARGS__)

#define PERFETTO_LOG(fmt, ...) PERFETTO_XLOG(kLogInfo, fmt, ##__VA_ARGS__)
#define PERFETTO_ILOG(fmt, ...) PERFETTO_XLOG(kLogImportant, fmt, ##__VA_ARGS__)
#define PERFETTO_ELOG(fmt, ...) PERFETTO_XLOG(kLogError, fmt, ##__VA_ARGS__)

#if PERFETTO_DCHECK_IS_ON()

#define PERFETTO_DLOG(fmt, ...) PERFETTO_XLOG(kLogDebug, fmt, ##__VA_ARGS__)

#define PERFETTO_DPLOG(x) \
  PERFETTO_DLOG("%s (errno: %d, %s)", (x), errno, strerror(errno))

#define PERFETTO_DCHECK(x)                             \
  do {                                                 \
    if (!__builtin_expect(!!(x), true)) {              \
      PERFETTO_DPLOG("PERFETTO_CHECK(" #x ")");        \
      *(reinterpret_cast<volatile int*>(0x10)) = 0x42; \
      __builtin_unreachable();                         \
    }                                                  \
  } while (0)

#else

#define PERFETTO_DLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#define PERFETTO_DPLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#define PERFETTO_DCHECK(x) ::perfetto::base::ignore_result(x)

#endif  // PERFETTO_DCHECK_IS_ON()

#if PERFETTO_DCHECK_IS_ON()
#define PERFETTO_CHECK(x) PERFETTO_DCHECK(x)
#else
#define PERFETTO_CHECK(x)                            \
  do {                                               \
    if (!__builtin_expect(!!(x), true)) {            \
      PERFETTO_ELOG("%s", "PERFETTO_CHECK(" #x ")"); \
      abort();                                       \
    }                                                \
  } while (0)

#endif  // PERFETTO_DCHECK_IS_ON()

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_LOGGING_H_
