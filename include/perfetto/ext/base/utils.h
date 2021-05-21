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

#ifndef INCLUDE_PERFETTO_EXT_BASE_UTILS_H_
#define INCLUDE_PERFETTO_EXT_BASE_UTILS_H_

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#include <atomic>
#include <string>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Even if Windows has errno.h, the all syscall-restart behavior does not apply.
// Trying to handle EINTR can cause more harm than good if errno is left stale.
// Chromium does the same.
#define PERFETTO_EINTR(x) (x)
#else
#define PERFETTO_EINTR(x)                                   \
  ([&] {                                                    \
    decltype(x) eintr_wrapper_result;                       \
    do {                                                    \
      eintr_wrapper_result = (x);                           \
    } while (eintr_wrapper_result == -1 && errno == EINTR); \
    return eintr_wrapper_result;                            \
  }())
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
using uid_t = unsigned int;
#if !PERFETTO_BUILDFLAG(PERFETTO_COMPILER_GCC)
using pid_t = unsigned int;
#endif
#if defined(_WIN64)
using ssize_t = int64_t;
#else
using ssize_t = long;
#endif
#endif

namespace perfetto {
namespace base {

constexpr uid_t kInvalidUid = static_cast<uid_t>(-1);
constexpr pid_t kInvalidPid = static_cast<pid_t>(-1);

// Do not add new usages of kPageSize, consider using GetSysPageSize() below.
// TODO(primiano): over time the semantic of kPageSize became too ambiguous.
// Strictly speaking, this constant is incorrect on some new devices where the
// page size can be 16K (e.g., crbug.com/1116576). Unfortunately too much code
// ended up depending on kPageSize for purposes that are not strictly related
// with the kernel's mm subsystem.
constexpr size_t kPageSize = 4096;

// Returns the system's page size. Use this when dealing with mmap, madvise and
// similar mm-related syscalls.
uint32_t GetSysPageSize();

template <typename T>
constexpr size_t ArraySize(const T& array) {
  return sizeof(array) / sizeof(array[0]);
}

// Function object which invokes 'free' on its parameter, which must be
// a pointer. Can be used to store malloc-allocated pointers in std::unique_ptr:
//
// std::unique_ptr<int, base::FreeDeleter> foo_ptr(
//     static_cast<int*>(malloc(sizeof(int))));
struct FreeDeleter {
  inline void operator()(void* ptr) const { free(ptr); }
};

template <typename T>
constexpr T AssumeLittleEndian(T value) {
#if !PERFETTO_IS_LITTLE_ENDIAN()
  static_assert(false, "Unimplemented on big-endian archs");
#endif
  return value;
}

// Round up |size| to a multiple of |alignment| (must be a power of two).
template <size_t alignment>
constexpr size_t AlignUp(size_t size) {
  static_assert((alignment & (alignment - 1)) == 0, "alignment must be a pow2");
  return (size + alignment - 1) & ~(alignment - 1);
}

inline bool IsAgain(int err) {
  return err == EAGAIN || err == EWOULDBLOCK;
}

// setenv(2)-equivalent. Deals with Windows vs Posix discrepancies.
void SetEnv(const std::string& key, const std::string& value);

// Calls mallopt(M_PURGE, 0) on Android. Does nothing on other platforms.
// This forces the allocator to release freed memory. This is used to work
// around various Scudo inefficiencies. See b/170217718.
void MaybeReleaseAllocatorMemToOS();

// geteuid() on POSIX OSes, returns 0 on Windows (See comment in utils.cc).
uid_t GetCurrentUserId();

// Forks the process.
// Parent: prints the PID of the child and exit(0).
// Child: redirects stdio onto /dev/null and chdirs into .
void Daemonize();

// Returns the path of the current executable, e.g. /foo/bar/exe.
std::string GetCurExecutablePath();

// Returns the directory where the current executable lives in, e.g. /foo/bar.
// This is independent of cwd().
std::string GetCurExecutableDir();

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_UTILS_H_
