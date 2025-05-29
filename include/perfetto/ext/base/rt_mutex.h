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

// Note: PERFETTO_HAS_RT_FUTEX() does not imply that we will use futexes. For
// now the usage of futexes is gated behind a runtime flag (for experiment
// rollout).
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_HAS_RT_FUTEX() true
#define PERFETTO_HAS_POSIX_RT_MUTEX() (__ANDROID_API__ >= 28)
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX)
#define PERFETTO_HAS_POSIX_RT_MUTEX() true
#define PERFETTO_HAS_RT_FUTEX() true
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_HAS_POSIX_RT_MUTEX() true
#define PERFETTO_HAS_RT_FUTEX() false
#else
#define PERFETTO_HAS_POSIX_RT_MUTEX() false
#define PERFETTO_HAS_RT_FUTEX() false
#endif

// On Android gettid() isn't actually a syscall but returns very quickly the
// tid stored in a TLS owned by Bionic.
// GLIBC instead doesn't seem to have this optimization and we do the TLS
// caching ourselves, because it makes a 4x difference to the uncontended case.
#define PERFETTO_GETTID_IS_FAST() PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <atomic>
#include <mutex>
#include <type_traits>

#if PERFETTO_HAS_POSIX_RT_MUTEX()
#include <pthread.h>
#endif

#if PERFETTO_HAS_RT_FUTEX()
#include <unistd.h>  // For gettid().
#endif

#include "perfetto/base/thread_annotations.h"
#include "perfetto/public/compiler.h"

namespace perfetto::base {

namespace internal {

#if PERFETTO_HAS_RT_FUTEX()
// A wrapper around PI Futexes. A futex is a wrapper around an atomic integer
// with an ABI shared with the kernel to handle the slowpath in the cases when
// the mutex is held, or we find out that there are waiters queued when we
// unlock. The operating principle is the following:
// - In the no-contention case, a futex boils down to an atomic
//   compare-and-exchange, without involving the kernel.
// - If a lock is contented at acquire time, we have to enter the kernel to
//   suspend our execution and join a wait chain.
// - It could still happen that we acquire the mutex via the fastpath (without
//   involving the kernel) but other waiters might queue up while we hold the
//   mutex. In that case the kernel will add a bit to the atomic int. That bit
//   will cause the unlock() compare-and-exchange to fail (because it no longer
//   matches our tid) which in turn will signal us to do a syscall to notify the
//   waiters.
class PERFETTO_LOCKABLE RtFutex {
 public:
  RtFutex() { PERFETTO_TSAN_MUTEX_CREATE(this, __tsan_mutex_not_static); }
  ~RtFutex() { PERFETTO_TSAN_MUTEX_DESTROY(this, __tsan_mutex_not_static); }

  // Disable copy or move. Copy doesn't make sense. Move isn't feasible because
  // the pointer to the atomic integer is the handle used by the kernel to setup
  // the wait chain. A movable futex would require the atomic integer to be heap
  // allocated, but that would create an indirection layer that is not needed in
  // most cases. If you really need a movable RtMutex, wrap it in a unique_ptr.
  RtFutex(const RtFutex&) = delete;
  RtFutex& operator=(const RtFutex&) = delete;
  RtFutex(RtFutex&&) = delete;
  RtFutex& operator=(RtFutex&&) = delete;

  inline bool TryLockFastpath() noexcept {
    int expected = 0;
    return lock_.compare_exchange_strong(expected, GetTid(),
                                         std::memory_order_acquire,
                                         std::memory_order_relaxed);
  }

  bool try_lock() noexcept PERFETTO_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    PERFETTO_TSAN_MUTEX_PRE_LOCK(this, __tsan_mutex_try_lock);
    if (PERFETTO_LIKELY(TryLockFastpath()) || TryLockSlowpath()) {
      PERFETTO_TSAN_MUTEX_POST_LOCK(this, __tsan_mutex_try_lock, 0);
      return true;
    }
    PERFETTO_TSAN_MUTEX_POST_LOCK(
        this, __tsan_mutex_try_lock | __tsan_mutex_try_lock_failed, 0);
    return false;
  }

  void lock() PERFETTO_EXCLUSIVE_LOCK_FUNCTION() {
    PERFETTO_TSAN_MUTEX_PRE_LOCK(this, 0);
    if (!PERFETTO_LIKELY(TryLockFastpath())) {
      LockSlowpath();
    }
    PERFETTO_TSAN_MUTEX_POST_LOCK(this, 0, 0);
  }

  void unlock() noexcept PERFETTO_UNLOCK_FUNCTION() {
    PERFETTO_TSAN_MUTEX_PRE_UNLOCK(this, 0);
    int expected = GetTid();
    // If the current value is our tid, we can unlock without a syscall since
    // there are no current waiters.
    if (!PERFETTO_LIKELY(lock_.compare_exchange_strong(
            expected, 0, std::memory_order_release,
            std::memory_order_relaxed))) {
      // The tid doesn't match because the kernel appended the FUTEX_WAITERS
      // bit. There are waiters, tell the kernel to notify them and unlock.
      UnlockSlowpath();
    }
    PERFETTO_TSAN_MUTEX_POST_UNLOCK(this, 0);
  }

 private:
  std::atomic<int> lock_{};

#if PERFETTO_GETTID_IS_FAST()
  static int GetTid() { return ::gettid(); }
#else
  static int GetTid();
#endif

  void LockSlowpath();
  bool TryLockSlowpath();
  void UnlockSlowpath();
};
#endif  // PERFETTO_HAS_RT_FUTEX

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
#if PERFETTO_HAS_RT_FUTEX()
using RtMutex = internal::RtFutex;
#elif PERFETTO_HAS_POSIX_RT_MUTEX()
using RtMutex = internal::RtPosixMutex;
#else
using RtMutex = std::mutex;
#endif

// This class is transitional and is used to retro-fit via flag rollout RtMutex
// in places where we used a standard std::mutex. New usages should use directly
// RtMutex. This class will be eventually deleted once the flag rollout sticks,
// and all the call sites will be replaced with RtMutex.
// TODO(primiano): remove this class once the rollout sticks.
class PERFETTO_LOCKABLE MaybeRtMutex {
  // This member variable reflects, at ctor time, the value of the global
  // static futex_flag. This is to ensure that if futex_flag is changed after
  // a RtMutex is created, we use the right set of APIs on that mutex.
  bool use_rt_mutex_ = enabled_flag.load(std::memory_order_relaxed);

 public:
  // Runtime flag to opt-in into using PI futexes rather than std::mutex.
  static constexpr bool kRtMutexDefaultFlagValue = false;
  static void SetEnableRtMutex(bool value) { enabled_flag = value; }

  MaybeRtMutex() noexcept {
    if (use_rt_mutex_) {
      new (&storage_) RtMutex();
    } else {
      new (&storage_) std::mutex();
    }
  }

  ~MaybeRtMutex() noexcept {
    if (use_rt_mutex_) {
      rt_mutex()->~RtMutex();
    } else {
      std_mutex()->~mutex();
    }
  }

  void lock() noexcept PERFETTO_EXCLUSIVE_LOCK_FUNCTION() {
    if (use_rt_mutex_) {
      rt_mutex()->lock();
    } else {
      std_mutex()->lock();
    }
  }

  void unlock() noexcept PERFETTO_UNLOCK_FUNCTION() {
    if (use_rt_mutex_) {
      rt_mutex()->unlock();
    } else {
      std_mutex()->unlock();
    }
  }

  bool try_lock() noexcept PERFETTO_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    if (use_rt_mutex_) {
      return rt_mutex()->try_lock();
    } else {
      return std_mutex()->try_lock();
    }
  }

 private:
  // A runtime flag that enables the usage of PI futexes when true, or falling
  // back on std::mutex when false (only on the supported Linux-based
  // platforms).
  static std::atomic<bool> enabled_flag;

  static constexpr size_t kMaxSize = sizeof(std::mutex) > sizeof(RtMutex)
                                         ? sizeof(std::mutex)
                                         : sizeof(RtMutex);
  static constexpr size_t kMaxAlign = alignof(std::mutex) > alignof(RtMutex)
                                          ? alignof(std::mutex)
                                          : alignof(RtMutex);

  std::aligned_storage_t<kMaxSize, kMaxAlign> storage_;

  std::mutex* std_mutex() { return reinterpret_cast<std::mutex*>(&storage_); }
  RtMutex* rt_mutex() { return reinterpret_cast<RtMutex*>(&storage_); }
};

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_RT_MUTEX_H_
