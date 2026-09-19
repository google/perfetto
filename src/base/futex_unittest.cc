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

#include <stdint.h>

#include <atomic>
#include <thread>

#include "perfetto/base/build_config.h"
#include "perfetto/base/time.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_HAS_FUTEX()
#include <sys/mman.h>
#include <unistd.h>

#include "perfetto/ext/base/subprocess.h"
#endif

namespace perfetto::base {
namespace {

TEST(FutexTest, HasFutexSupportReturnsExpected) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX_BUT_NOT_QNX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  EXPECT_TRUE(HasFutexSupport());
#else
  EXPECT_FALSE(HasFutexSupport());
#endif
}

TEST(FutexTest, WaitReturnsValueMismatchIfAlreadyDifferent) {
  if (!HasFutexSupport())
    GTEST_SKIP() << "Futex not available on this platform";

  uint32_t word = 42;
  auto result = FutexWait(&word, 0, 30000);
  EXPECT_EQ(result, FutexWaitResult::kValueMismatch);
}

TEST(FutexTest, WaitTimesOut) {
  if (!HasFutexSupport())
    GTEST_SKIP() << "Futex not available on this platform";

  uint32_t word = 0;
  const auto before = GetWallTimeMs();
  auto result = FutexWait(&word, 0, 1);
  const auto elapsed = GetWallTimeMs() - before;

  EXPECT_EQ(result, FutexWaitResult::kTimedOut);
  EXPECT_LT(elapsed.count(), 5000);
}

TEST(FutexTest, WakeWithoutWaitersSucceeds) {
  if (!HasFutexSupport())
    GTEST_SKIP() << "Futex not available on this platform";

  uint32_t word = 0;
  int woken = FutexWake(&word, 1);
  EXPECT_EQ(woken, 0);
}

TEST(FutexTest, WaiterWokenByWake) {
  if (!HasFutexSupport())
    GTEST_SKIP() << "Futex not available on this platform";

  std::atomic<uint32_t> word{0};
  std::atomic<bool> waiter_done{false};
  FutexWaitResult waiter_result = FutexWaitResult::kError;

  std::thread waiter([&] {
    waiter_result = FutexWait(reinterpret_cast<uint32_t*>(&word), 0, 30000);
    waiter_done.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  word.store(1, std::memory_order_relaxed);
  FutexWake(reinterpret_cast<uint32_t*>(&word), 1);

  waiter.join();
  EXPECT_TRUE(waiter_done.load());
  EXPECT_TRUE(waiter_result == FutexWaitResult::kWoken ||
              waiter_result == FutexWaitResult::kValueMismatch);
}

#if PERFETTO_HAS_FUTEX()
TEST(FutexTest, CrossProcessWakeup) {
  void* shared =
      mmap(nullptr, sizeof(std::atomic<uint32_t>), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(shared, MAP_FAILED);

  auto* word = new (shared) std::atomic<uint32_t>(0);
  uint32_t* addr = reinterpret_cast<uint32_t*>(word);

  {
    Subprocess child;
    child.args.posix_entrypoint_for_testing = [addr] {
      const auto deadline = GetWallTimeMs() + std::chrono::seconds(30);
      while (GetWallTimeMs() < deadline) {
        int woken = FutexWake(addr, 1);
        if (woken != 0)
          _exit(woken == 1 ? 0 : 1);
        SleepMicroseconds(1000);
      }
      _exit(1);
    };
    child.Start();

    EXPECT_EQ(FutexWait(addr, 0, 30000), FutexWaitResult::kWoken);
    EXPECT_EQ(word->load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(child.Wait(30000));
    EXPECT_EQ(child.returncode(), 0);
  }

  munmap(shared, sizeof(std::atomic<uint32_t>));
}
#endif  // PERFETTO_HAS_FUTEX()

}  // namespace
}  // namespace perfetto::base
