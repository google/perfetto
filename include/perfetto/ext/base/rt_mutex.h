/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_RT_MUTEX_H_
#define INCLUDE_PERFETTO_EXT_BASE_RT_MUTEX_H_

// This header introduces the RtMutex class for priority-inheritance mutexes.
// RtMutex is NOT a blanket replacement for std::mutex and should be used only
// in cases where we know we are expect to be used on a RT thread.
// In the contended case RtMutex is generally slower than a std::mutex (or any
// non-RT implementation).
// Under the hoods this class does the following:
// - Linux/Android: it uses PI futexes.
// - MacOS/iOS: it uses pthread_mutex with PTHREAD_PRIO_INHERIT.
// - Other platforms: falls back on a standard std::mutex. On Windows 11+
//   std::mutex has effectively PI semantics due to AutoBoost
//   https://github.com/MicrosoftDocs/win32/commit/a43cb3b5039c5cfc53642bfcea174003a2f1168f

#include "perfetto/base/build_config.h"
#include "perfetto/base/thread_annotations.h"
#include "perfetto/ext/base/flags.h"
#include "perfetto/public/compiler.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_HAS_POSIX_RT_MUTEX() true
#else
#define PERFETTO_HAS_POSIX_RT_MUTEX() false
#endif

#include <atomic>
#include <mutex>
#include <type_traits>

#if PERFETTO_HAS_POSIX_RT_MUTEX()
#include <pthread.h>
#endif

namespace perfetto::base {

namespace internal {

#if PERFETTO_HAS_POSIX_RT_MUTEX()
class PERFETTO_LOCKABLE RtPosixMutex {
 public:
  RtPosixMutex() noexcept;
  ~RtPosixMutex() noexcept;

  RtPosixMutex(const RtPosixMutex&) = delete;
  RtPosixMutex& operator=(const RtPosixMutex&) = delete;
  RtPosixMutex(RtPosixMutex&&) = delete;
  RtPosixMutex& operator=(RtPosixMutex&&) = delete;

  bool try_lock() noexcept PERFETTO_EXCLUSIVE_TRYLOCK_FUNCTION(true);
  void lock() noexcept PERFETTO_EXCLUSIVE_LOCK_FUNCTION();
  void unlock() noexcept PERFETTO_UNLOCK_FUNCTION();

 private:
  pthread_mutex_t mutex_;
};

#endif  // PERFETTO_HAS_POSIX_RT_MUTEX
}  // namespace internal

// Pick the best implementation for the target platform.
// See comments in the top of the doc.
#if PERFETTO_HAS_POSIX_RT_MUTEX()
using RtMutex = internal::RtPosixMutex;
#else
using RtMutex = std::mutex;
#endif

using MaybeRtMutex =
    std::conditional_t<base::flags::use_rt_mutex, RtMutex, std::mutex>;

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_RT_MUTEX_H_
