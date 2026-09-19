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

// Futex operations wait and wake threads on a 32-bit word in memory. A waiter
// sleeps if the word matches an expected value. Another thread can update the
// word and wake waiters to recheck their condition.
//
// These operations also work across processes when the word is in shared
// memory. Each process uses its own address for the same backing storage.
//
// Neither operation modifies the word or provides C++ memory ordering. Callers
// must use atomic accesses and the memory ordering required by their protocol.
// To avoid lost wakeups, update the word before calling FutexWake().
// Wakeups are not remembered for subsequent waits.
//
// Implemented on Linux and Android. Use HasFutexSupport() to select a fallback
// on other platforms.

// Returns whether this build implements FutexWait() and FutexWake(). Does not
// check runtime restrictions, such as a sandbox that blocks the system call.
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
  kValueMismatch,
  // The relative timeout expired.
  kTimedOut,
  // The wait was interrupted, for example by a signal. It was not retried.
  kInterrupted,
  // The operation failed, including on unsupported platforms. May indicate
  // invalid arguments or runtime restrictions. Do not blindly retry.
  kError,
};

// Makes one attempt to sleep if |word| contains |expected_value|. The value
// check and the start of the wait are atomic with respect to FutexWake().
// Returns kValueMismatch without sleeping if the value does not match.
//
// |timeout_ms| is a relative timeout in milliseconds. Zero causes no wait.
// Scheduling can delay the return beyond the timeout.
//
// Recheck the caller's condition after every result other than kError,
// including kWoken. Interruptions and spurious wakes are not retried. A retry
// loop with an overall deadline must compute the remaining timeout each time.
//
// |word| must point to a readable, 4-byte-aligned word. Keep the storage mapped
// until the call returns. Do not reuse the storage during the call.
// The address is passed to the OS without C++ loads or stores. If |word| points
// into an atomic object, verify its layout and update behavior: the OS must be
// able to read that 32-bit word atomically during each update.
//
// Returns kError without sleeping on unsupported platforms.
FutexWaitResult FutexWait(uint32_t* word,
                          uint32_t expected_value,
                          uint32_t timeout_ms);

// Wakes up to |max_waiters| threads waiting on |word|, including waiters in
// other processes that share the same storage.
// |max_waiters| must be positive. Use INT_MAX to wake all waiters.
//
// |word| has the same storage requirements as FutexWait().
// Returns the number of waiters woken, or -1 on failure or unsupported
// platforms.
int FutexWake(uint32_t* word, int max_waiters);

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_FUTEX_H_
