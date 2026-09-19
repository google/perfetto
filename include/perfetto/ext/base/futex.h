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

#ifndef INCLUDE_PERFETTO_EXT_BASE_FUTEX_H_
#define INCLUDE_PERFETTO_EXT_BASE_FUTEX_H_

#include <stdint.h>

#include "perfetto/base/build_config.h"

namespace perfetto::base {

// These functions wait and wake threads on a 32-bit word in memory. A futex
// lets a thread sleep while the word has an expected value. Another thread
// changes the word and wakes waiters so they can recheck their condition.
//
// These operations also work across processes when the word is in shared
// memory. Each process uses its own address for the same backing storage.
//
// Neither operation modifies the word or provides C++ memory ordering. Callers
// must use atomic accesses and the memory ordering required by their protocol.
// Publish a change before you wake waiters. A wake has no effect on future
// waits.
//
// These functions support Linux and Android. Other platforms return an
// unavailable result without a wait or wake. Use HasFutexSupport() to select a
// fallback.

// This function returns whether the build implements FutexWait() and
// FutexWake(). The result is a compile-time constant. It does not check runtime
// restrictions, such as a sandbox that blocks the system call.
constexpr bool HasFutexSupport() {
  // TODO(rsavitski): Add support for other platforms.
  return PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) ||
         PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID);
}

enum class FutexWaitResult {
  // The wait returned after a wake, which may be spurious. The word need not
  // have changed.
  kWoken,
  // The word did not match the expected value when checked before sleeping.
  kValueChanged,
  // The relative timeout expired.
  kTimedOut,
  // The wait was interrupted, for example by a signal. It was not retried.
  kInterrupted,
  // This build does not support futexes, or the operation failed. The caller
  // should use a fallback instead of retrying the wait.
  kUnavailable,
};

// This function makes one attempt to sleep while |word| contains
// |expected_value|. The value check and the start of the wait are atomic with
// respect to FutexWake(). If the value does not match, the function returns
// without a wait.
//
// |timeout_ms| is a relative timeout in milliseconds. Zero causes no wait.
// Scheduling can delay the return beyond the timeout.
//
// Recheck the caller's condition after every normal return, including kWoken.
// This function does not retry interruptions or spurious wakes. A retry loop
// with an overall deadline must compute the remaining timeout for each call.
//
// |word| must point to a readable, 4-byte-aligned word. Keep the storage mapped
// until the call returns. Do not reuse the storage during the call.
// This function passes the address to the OS without C++ loads or stores.
// If |word| points into an atomic object, verify its layout and update
// behavior. The OS must be able to read that 32-bit word atomically during each
// update.
//
// On unsupported platforms, this function returns kUnavailable without a wait.
FutexWaitResult FutexWait(uint32_t* word,
                          uint32_t expected_value,
                          uint32_t timeout_ms);

// This function wakes up to |max_waiters| threads that wait on |word|.
// This includes waiters in other processes that share the same storage.
// |max_waiters| must be positive. Use INT_MAX to wake all waiters.
//
// This function does not change the value of the word.
// |word| has the same storage requirements as FutexWait().
// The return value is the number of waiters woken, or -1 if the operation fails
// or the platform is unsupported.
int FutexWake(uint32_t* word, int max_waiters);

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_FUTEX_H_
