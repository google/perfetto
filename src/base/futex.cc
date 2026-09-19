/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/ext/base/futex.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <errno.h>
#include <linux/futex.h>
#include <linux/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace perfetto::base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

// Use the shared FUTEX_WAIT/FUTEX_WAKE operations so different mappings of the
// same shared-memory word refer to the same wait queue across processes.

FutexWaitResult FutexWait(uint32_t* word,
                          uint32_t expected_value,
                          uint32_t timeout_ms) {
  // SYS_futex expects seconds and nanoseconds in two kernel-sized longs.
  // Use that layout directly because libc's timespec can use a wider time_t.
  // Even UINT32_MAX milliseconds fits in signed 32-bit seconds.
  const __kernel_long_t timeout[] = {
      static_cast<__kernel_long_t>(timeout_ms / 1000),
      static_cast<__kernel_long_t>((timeout_ms % 1000) * 1000000)};
  const int ret = static_cast<int>(syscall(
      SYS_futex, word, FUTEX_WAIT, expected_value, timeout, nullptr, 0));
  if (ret == 0)
    return FutexWaitResult::kWoken;

  switch (errno) {
    case EAGAIN:
      return FutexWaitResult::kValueMismatch;
    case ETIMEDOUT:
      return FutexWaitResult::kTimedOut;
    case EINTR:
      return FutexWaitResult::kInterrupted;
    default:
      PERFETTO_DPLOG("futex FUTEX_WAIT failed");
      return FutexWaitResult::kError;
  }
}

int FutexWake(uint32_t* word, int max_waiters) {
  PERFETTO_DCHECK(max_waiters > 0);
  const int ret = static_cast<int>(
      syscall(SYS_futex, word, FUTEX_WAKE, max_waiters, nullptr, nullptr, 0));
  if (PERFETTO_UNLIKELY(ret < 0)) {
    PERFETTO_DPLOG("futex FUTEX_WAKE failed");
    return -1;
  }
  return ret;
}

#else

FutexWaitResult FutexWait(uint32_t* word,
                          uint32_t expected_value,
                          uint32_t timeout_ms) {
  ignore_result(word);
  ignore_result(expected_value);
  ignore_result(timeout_ms);
  return FutexWaitResult::kError;
}

int FutexWake(uint32_t* word, int max_waiters) {
  ignore_result(word);
  ignore_result(max_waiters);
  return -1;
}

#endif

}  // namespace perfetto::base
