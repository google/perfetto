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

#include "perfetto/ext/base/rt_mutex.h"

#include <errno.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <dlfcn.h>
#endif

namespace perfetto::base {

namespace internal {

#if PERFETTO_HAS_POSIX_RT_MUTEX()

RtPosixMutex::RtPosixMutex() noexcept {
  pthread_mutexattr_t at{};
  PERFETTO_CHECK(pthread_mutexattr_init(&at) == 0);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && __ANDROID_API__ < 28
  // pthread_mutexattr_setprotocol is only available on API 28.
  using SetprotocolFuncT = int (*)(pthread_mutexattr_t*, int);
  static auto setprotocol_func = reinterpret_cast<SetprotocolFuncT>(
      dlsym(RTLD_DEFAULT, "pthread_mutexattr_setprotocol"));
  if (setprotocol_func) {
    PERFETTO_CHECK(setprotocol_func(&at, PTHREAD_PRIO_INHERIT) == 0);
  } else {
    static uint64_t log_once = 0;
    if (log_once++ == 0) {
      PERFETTO_LOG(
          "Priority-inheritance RtMutex is not available in this version of "
          "Android.");
    }
  }
#else  // Not Android (but POSIX RT)
  PERFETTO_CHECK(pthread_mutexattr_setprotocol(&at, PTHREAD_PRIO_INHERIT) == 0);
#endif
  PERFETTO_CHECK(pthread_mutex_init(&mutex_, &at) == 0);
}

RtPosixMutex::~RtPosixMutex() noexcept {
  pthread_mutex_destroy(&mutex_);
}

bool RtPosixMutex::try_lock() noexcept {
  int res = pthread_mutex_trylock(&mutex_);
  if (res == 0)
    return true;
  // NOTE: Unlike most Linux APIs, pthread_mutex_trylock "returns" the error
  // code, it does NOT use errno.
  if (res == EBUSY)
    return false;
  PERFETTO_FATAL("pthread_mutex_trylock() failed");
}

void RtPosixMutex::lock() noexcept {
  PERFETTO_CHECK(pthread_mutex_lock(&mutex_) == 0);
}

void RtPosixMutex::unlock() noexcept {
  PERFETTO_CHECK(pthread_mutex_unlock(&mutex_) == 0);
}

#endif  // PERFETTO_HAS_POSIX_RT_MUTEX

}  // namespace internal
}  // namespace perfetto::base
