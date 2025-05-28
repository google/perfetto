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

#if PERFETTO_HAS_RT_FUTEX()
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif  // PERFETTO_HAS_RT_FUTEX

namespace perfetto::base {

std::atomic<bool> MaybeRtMutex::enabled_flag =
    MaybeRtMutex::kRtMutexDefaultFlagValue;

namespace internal {

#if PERFETTO_HAS_RT_FUTEX()

void RtFutex::LockSlowpath() {
  auto res = PERFETTO_EINTR(
      syscall(SYS_futex, &lock_, FUTEX_LOCK_PI_PRIVATE, 0, nullptr));
  PERFETTO_CHECK(res == 0);
}

bool RtFutex::TryLockSlowpath() {
  auto res = PERFETTO_EINTR(
      syscall(SYS_futex, &lock_, FUTEX_TRYLOCK_PI_PRIVATE, 0, nullptr));
  if (res == 0)
    return true;
  if (errno == EBUSY || errno == EDEADLK)
    return false;
  PERFETTO_FATAL("FUTEX_TRYLOCK_PI_PRIVATE failed");
}

void RtFutex::UnlockSlowpath() {
  auto res = PERFETTO_EINTR(
      syscall(SYS_futex, &lock_, FUTEX_UNLOCK_PI_PRIVATE, 0, nullptr));
  PERFETTO_CHECK(res == 0);
}

#if !PERFETTO_GETTID_IS_FAST()
namespace {
thread_local int g_cached_tid = -1;
}

int RtFutex::GetTid() {
  if (PERFETTO_UNLIKELY(g_cached_tid == -1)) {
    static int register_atfork_once = [] {
      return pthread_atfork(/* prepare = */ nullptr,
                            /* parent  = */ nullptr,
                            /* child   = */ [] { g_cached_tid = -1; });
    }();
    base::ignore_result(register_atfork_once);
    g_cached_tid = static_cast<int>(syscall(SYS_gettid));
  }
  return g_cached_tid;
}
#endif  // !PERFETTO_GETTID_IS_FAST

#endif  // PERFETTO_HAS_RT_FUTEX

#if PERFETTO_HAS_POSIX_RT_MUTEX()

RtPosixMutex::RtPosixMutex() noexcept {
  pthread_mutexattr_t at{};
  PERFETTO_CHECK(pthread_mutexattr_init(&at) == 0);
  PERFETTO_CHECK(pthread_mutexattr_setprotocol(&at, PTHREAD_PRIO_INHERIT) == 0);
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
