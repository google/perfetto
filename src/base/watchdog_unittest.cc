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

#include "perfetto/base/watchdog.h"

#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/page_allocator.h"

#include <time.h>
#include <map>

namespace perfetto {
namespace base {
namespace {

class TestWatchdog : public Watchdog {
 public:
  explicit TestWatchdog(uint32_t polling_interval_ms)
      : Watchdog(polling_interval_ms) {}
  ~TestWatchdog() override {}
  TestWatchdog(TestWatchdog&& other) noexcept = default;
};

TEST(WatchdogTest, TimerCrash) {
  // Create a timer for 20 ms and don't release wihin the time.
  EXPECT_DEATH(
      {
        TestWatchdog watchdog(100);
        auto handle = watchdog.CreateFatalTimer(20);
        usleep(200 * 1000);
      },
      "");
}

TEST(WatchdogTest, CrashEvenWhenMove) {
  std::map<int, Watchdog::Timer> timers;
  EXPECT_DEATH(
      {
        TestWatchdog watchdog(100);
        timers.emplace(0, watchdog.CreateFatalTimer(20));
        usleep(200 * 1000);
      },
      "");
}

TEST(WatchdogTest, CrashMemory) {
  EXPECT_DEATH(
      {
        // Allocate 8MB of data and use it to increase RSS.
        const size_t kSize = 8 * 1024 * 1024;
        auto void_ptr = PageAllocator::Allocate(kSize);
        volatile uint8_t* ptr = static_cast<volatile uint8_t*>(void_ptr.get());
        for (size_t i = 0; i < kSize; i += sizeof(size_t)) {
          *reinterpret_cast<volatile size_t*>(&ptr[i]) = i;
        }

        TestWatchdog watchdog(5);
        watchdog.SetMemoryLimit(8 * 1024 * 1024, 25);
        watchdog.Start();

        // Sleep so that the watchdog has some time to pick it up.
        usleep(1000 * 1000);
      },
      "");
}

TEST(WatchdogTest, CrashCpu) {
  EXPECT_DEATH(
      {
        TestWatchdog watchdog(1);
        watchdog.SetCpuLimit(10, 25);
        watchdog.Start();
        volatile int x = 0;
        while (true) {
          x++;
        }
      },
      "");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
