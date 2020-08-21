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
#include <stdlib.h>
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <sys/types.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <unistd.h>  // For getpagesize().
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <mach/vm_page_size.h>
#endif

#include <atomic>

#define PERFETTO_EINTR(x)                                   \
  ([&] {                                                    \
    decltype(x) eintr_wrapper_result;                       \
    do {                                                    \
      eintr_wrapper_result = (x);                           \
    } while (eintr_wrapper_result == -1 && errno == EINTR); \
    return eintr_wrapper_result;                            \
  }())

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// TODO(brucedawson) - create a ::perfetto::base::IOSize to replace this.
#if defined(_WIN64)
using ssize_t = __int64;
#else
using ssize_t = long;
#endif
#endif

namespace perfetto {
namespace base {

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
constexpr uid_t kInvalidUid = static_cast<uid_t>(-1);
constexpr pid_t kInvalidPid = static_cast<pid_t>(-1);
#endif

// Do not add new usages of kPageSize, consider using GetSysPageSize() below.
// TODO(primiano): over time the semantic of kPageSize became too ambiguous.
// Strictly speaking, this constant is incorrect on some new devices where the
// page size can be 16K (e.g., crbug.com/1116576). Unfortunately too much code
// ended up depending on kPageSize for purposes that are not strictly related
// with the kernel's mm subsystem.
constexpr size_t kPageSize = 4096;

// Returns the system's page size. Use this when dealing with mmap, madvise and
// similar mm-related syscalls.
inline uint32_t GetSysPageSize() {
  ignore_result(kPageSize);  // Just to keep the amalgamated build happy.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  static std::atomic<uint32_t> page_size{0};
  // This function might be called in hot paths. Avoid calling getpagesize() all
  // the times, in many implementations getpagesize() calls sysconf() which is
  // not cheap.
  uint32_t cached_value = page_size.load(std::memory_order_relaxed);
  if (PERFETTO_UNLIKELY(cached_value == 0)) {
    cached_value = static_cast<uint32_t>(getpagesize());
    page_size.store(cached_value, std::memory_order_relaxed);
  }
  return cached_value;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return static_cast<uint32_t>(vm_page_size);
#else
  return 4096;
#endif
}

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
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                "Unimplemented on big-endian archs");
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

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_UTILS_H_
