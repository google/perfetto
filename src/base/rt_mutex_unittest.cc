/*
 * Copyright (C) 2021 The Android Open Source Project
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
#include "perfetto/ext/base/flags.h"

#include "test/gtest_and_gmock.h"

#include <thread>

namespace perfetto {
namespace base {
namespace {

template <typename T>
class RtMutexTest : public testing::Test {
 public:
  using MutexType = T;
};

using RtMutexTestTypes = testing::Types<std::mutex
#if PERFETTO_HAS_POSIX_RT_MUTEX()
                                        ,
                                        internal::RtPosixMutex
#endif
#if PERFETTO_HAS_RT_FUTEX()
                                        ,
                                        internal::RtFutex
#endif
                                        >;

class NameGenerator {
 public:
  template <typename T>
  static std::string GetName(int) {
    if constexpr (std::is_same_v<T, std::mutex>)
      return "StdMutex";
#if PERFETTO_HAS_POSIX_RT_MUTEX()
    if constexpr (std::is_same_v<T, internal::RtPosixMutex>)
      return "RtPosix";
#endif
#if PERFETTO_HAS_RT_FUTEX()
    if constexpr (std::is_same_v<T, internal::RtFutex>)
      return "RtFutex";
#endif
  }
};

TYPED_TEST_SUITE(RtMutexTest, RtMutexTestTypes, NameGenerator);

TYPED_TEST(RtMutexTest, LockUnlock) {
  typename TestFixture::MutexType m1, m2;
  m1.lock();
  EXPECT_FALSE(m1.try_lock());
  bool m2_locked = m2.try_lock();
  EXPECT_TRUE(m2_locked);

  if (m2_locked)  // This is always true, just to keep the compiler happy.
    m2.unlock();
  m1.unlock();

  bool m1_locked = m1.try_lock();
  m2_locked = m2.try_lock();
  EXPECT_TRUE(m1_locked);
  EXPECT_TRUE(m2_locked);

  if (m1_locked)
    m1.unlock();
  if (m2_locked)
    m2.unlock();
}

TYPED_TEST(RtMutexTest, UniqueLock) {
  typename TestFixture::MutexType m1, m2;

  {
    std::unique_lock<typename TestFixture::MutexType> l1(m1);
    EXPECT_TRUE(l1.owns_lock());
  }

  {
    std::unique_lock<typename TestFixture::MutexType> l2(m2);
    EXPECT_TRUE(l2.owns_lock());
  }
}

// This test checks whether the custom mutex enforces correct memory ordering
// (i.e., acquire on lock, release on unlock). Without proper ordering, it's
// possible for a reader thread to observe stale values due to hardware-level
// reordering—especially on weak memory architectures like ARM.
//
// Specifically:
// Thread A sets x=1 then y=1 inside the critical section.
// Thread B, also inside a critical section, reads y then x.
// It should never observe y==1 && x==0 if the mutex enforces the correct
// ordering.
//
// The reset to 0 is also done under lock to ensure correct matching.
TYPED_TEST(RtMutexTest, AcquireReleaseSemantics) {
  constexpr int kIterations = 10000;
  typename TestFixture::MutexType mutex;
  volatile std::atomic<int> x{0}, y{0}, error_count{0};

  auto writer = [&]() {
    for (int i = 0; i < kIterations; ++i) {
      mutex.lock();
      x.store(1, std::memory_order_relaxed);
      y.store(1, std::memory_order_relaxed);
      x.store(0, std::memory_order_relaxed);
      y.store(0, std::memory_order_relaxed);
      mutex.unlock();
    }
  };

  auto reader = [&]() {
    for (int i = 0; i < kIterations; ++i) {
      mutex.lock();
      int y_val = y.load(std::memory_order_relaxed);
      int x_val = x.load(std::memory_order_relaxed);
      mutex.unlock();

      if (y_val == 1 && x_val == 0) {
        error_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::thread t1(writer);
  std::thread t2(reader);
  t1.join();
  t2.join();

  EXPECT_EQ(error_count.load(), 0);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
