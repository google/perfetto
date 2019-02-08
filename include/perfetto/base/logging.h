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
#include <string.h>  // For strerror.

#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
#define PERFETTO_DCHECK_IS_ON() 0
#else
#define PERFETTO_DCHECK_IS_ON() 1
#endif

#if !defined(PERFETTO_FORCE_DLOG)
#define PERFETTO_DLOG_IS_ON() PERFETTO_DCHECK_IS_ON()
#else
#define PERFETTO_DLOG_IS_ON() PERFETTO_FORCE_DLOG
#endif

#include "perfetto/base/build_config.h"
#include "perfetto/base/utils.h"

#if defined(PERFETTO_ANDROID_ASYNC_SAFE_LOG)
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    !PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#error "Async-safe logging is limited to Android tree builds"
#endif
// For binaries which need a very lightweight logging implementation.
// Note that this header is incompatible with android/log.h.
#include <async_safe/log.h>
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Normal android logging.
#include <android/log.h>
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <unistd.h>
#endif

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

#define PERFETTO_LOG_LINE__(x) #x
#define PERFETTO_LOG_LINE_(x) PERFETTO_LOG_LINE__(x)
#define PERFETTO_LOG_LINE PERFETTO_LOG_LINE_(__LINE__)

enum LogLev { kLogDebug = 0, kLogInfo, kLogImportant, kLogError };
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) || PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
// The escape sequences don't work in a Windows command prompt.
#define PERFETTO_XLOG_STDERR(level, fmt, ...)                              \
  fprintf(stderr, "%-24.24s " fmt "\n",                                    \
          ::perfetto::base::Basename(__FILE__ "(" PERFETTO_LOG_LINE "):"), \
          ##__VA_ARGS__)
#else
constexpr const char* kLogFmt[] = {"\x1b[2m", "\x1b[39m", "\x1b[32m\x1b[1m",
                                   "\x1b[31m"};

#define PERFETTO_XLOG_STDERR(level, fmt, ...)                         \
  fprintf(stderr, "\x1b[90m%-24.24s\x1b[0m %s" fmt "\x1b[0m\n",       \
          ::perfetto::base::Basename(__FILE__ ":" PERFETTO_LOG_LINE), \
          ::perfetto::base::kLogFmt[::perfetto::base::LogLev::level], \
          ##__VA_ARGS__)
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    defined(PERFETTO_ANDROID_ASYNC_SAFE_LOG)
#define PERFETTO_XLOG(level, fmt, ...)                                         \
  do {                                                                         \
    async_safe_format_log(                                                     \
        (ANDROID_LOG_DEBUG + ::perfetto::base::LogLev::level), "perfetto",     \
        "%s " fmt, ::perfetto::base::Basename(__FILE__ ":" PERFETTO_LOG_LINE), \
        ##__VA_ARGS__);                                                        \
  } while (0)
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Standard logging marco on Android - log to both stderr and logcat. When part
// of the Android tree, stderr points to /dev/null so logcat is the only way to
// get some logging.
#define PERFETTO_XLOG(level, fmt, ...)                                         \
  do {                                                                         \
    __android_log_print(                                                       \
        (ANDROID_LOG_DEBUG + ::perfetto::base::LogLev::level), "perfetto",     \
        "%s " fmt, ::perfetto::base::Basename(__FILE__ ":" PERFETTO_LOG_LINE), \
        ##__VA_ARGS__);                                                        \
    PERFETTO_XLOG_STDERR(level, fmt, ##__VA_ARGS__);                           \
  } while (0)
#else
#define PERFETTO_XLOG PERFETTO_XLOG_STDERR
#endif

#define PERFETTO_IMMEDIATE_CRASH() \
  do {                             \
    __builtin_trap();              \
    __builtin_unreachable();       \
  } while (0)

#define PERFETTO_LOG(fmt, ...) PERFETTO_XLOG(kLogInfo, fmt, ##__VA_ARGS__)
#define PERFETTO_ILOG(fmt, ...) PERFETTO_XLOG(kLogImportant, fmt, ##__VA_ARGS__)
#define PERFETTO_ELOG(fmt, ...) PERFETTO_XLOG(kLogError, fmt, ##__VA_ARGS__)
#define PERFETTO_FATAL(fmt, ...)       \
  do {                                 \
    PERFETTO_PLOG(fmt, ##__VA_ARGS__); \
    PERFETTO_IMMEDIATE_CRASH();        \
  } while (0)

#define PERFETTO_PLOG(x, ...) \
  PERFETTO_ELOG(x " (errno: %d, %s)", ##__VA_ARGS__, errno, strerror(errno))

#if PERFETTO_DLOG_IS_ON()

#define PERFETTO_DLOG(fmt, ...) PERFETTO_XLOG(kLogDebug, fmt, ##__VA_ARGS__)

#define PERFETTO_DPLOG(x, ...) \
  PERFETTO_DLOG(x " (errno: %d, %s)", ##__VA_ARGS__, errno, strerror(errno))

#else

#define PERFETTO_DLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)
#define PERFETTO_DPLOG(...) ::perfetto::base::ignore_result(__VA_ARGS__)

#endif  // PERFETTO_DLOG_IS_ON()

#if PERFETTO_DCHECK_IS_ON()

#define PERFETTO_DCHECK(x)                           \
  do {                                               \
    if (PERFETTO_UNLIKELY(!(x))) {                   \
      PERFETTO_PLOG("%s", "PERFETTO_CHECK(" #x ")"); \
      PERFETTO_IMMEDIATE_CRASH();                    \
    }                                                \
  } while (0)

#define PERFETTO_DFATAL(fmt, ...)      \
  do {                                 \
    PERFETTO_PLOG(fmt, ##__VA_ARGS__); \
    PERFETTO_IMMEDIATE_CRASH();        \
  } while (0)

#else

#define PERFETTO_DCHECK(x) \
  do {                     \
  } while (false && (x))

#define PERFETTO_DFATAL(...) ::perfetto::base::ignore_result(__VA_ARGS__)

#endif  // PERFETTO_DCHECK_IS_ON()

#if PERFETTO_DCHECK_IS_ON()
#define PERFETTO_CHECK(x) PERFETTO_DCHECK(x)
#else
#define PERFETTO_CHECK(x)                            \
  do {                                               \
    if (PERFETTO_UNLIKELY(!(x))) {                   \
      PERFETTO_PLOG("%s", "PERFETTO_CHECK(" #x ")"); \
      PERFETTO_IMMEDIATE_CRASH();                    \
    }                                                \
  } while (0)

#endif  // PERFETTO_DCHECK_IS_ON()

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_LOGGING_H_
