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

#include "perfetto/base/logging.h"

#include "gtest/gtest.h"

#include <time.h>

namespace perfetto {
namespace base {
namespace {

TEST(WatchDogTest, Crash) {
  EXPECT_DEATH(
      {
        WatchDog watchdog(1);
        int sleep_s = 20;
        while (sleep_s != 0) {
          sleep_s = sleep(sleep_s);
        }
      },
      "");
}

TEST(WatchDogTest, NoCrash) {
  WatchDog watchdog(100000);
  PERFETTO_CHECK(usleep(5000) != -1);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
