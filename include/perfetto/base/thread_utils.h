/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_BASE_THREAD_UTILS_H_
#define INCLUDE_PERFETTO_BASE_THREAD_UTILS_H_

#include <stdint.h>

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <processthreadsapi.h>
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <zircon/process.h>
#include <zircon/types.h>
#else
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
using PlatformThreadID = pid_t;
inline PlatformThreadID GetThreadId() {
  return gettid();
}
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)
using PlatformThreadID = pid_t;
inline PlatformThreadID GetThreadId() {
  return static_cast<pid_t>(syscall(__NR_gettid));
}
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
using PlatformThreadID = zx_handle_t;
inline PlatformThreadID GetThreadId() {
  return static_cast<pid_t>(zx_thread_self());
}
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
using PlatformThreadID = uint64_t;
inline PlatformThreadID GetThreadId() {
  uint64_t tid;
  pthread_threadid_np(nullptr, &tid);
  return tid;
}
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
using PlatformThreadID = uint64_t;
inline PlatformThreadID GetThreadId() {
  return static_cast<uint64_t>(GetCurrentThreadID());
}
#else  // Default to pthreads in case no OS is set.
using PlatformThreadID = pthread_t;
inline PlatformThreadID GetThreadId() {
  return pthread_self();
}
#endif

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_THREAD_UTILS_H_
