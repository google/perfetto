/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/base/time.h"

#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(TimeTest, Conversions) {
  TimeMillis ms = GetWallTimeMs();
  TimeNanos ns = GetWallTimeNs();
  EXPECT_NEAR(static_cast<double>(ms.count()),
              static_cast<double>(ns.count()) / 1000000, 1000);

  {
    struct timespec ts = ToPosixTimespec(TimeMillis(0));
    EXPECT_EQ(0, ts.tv_sec);
    EXPECT_EQ(0, ts.tv_nsec);
  }
  {
    struct timespec ts = ToPosixTimespec(TimeMillis(1));
    EXPECT_EQ(0, ts.tv_sec);
    EXPECT_EQ(1000000, ts.tv_nsec);
  }
  {
    struct timespec ts = ToPosixTimespec(TimeMillis(12345));
    EXPECT_EQ(12, ts.tv_sec);
    EXPECT_EQ(345000000, ts.tv_nsec);
  }
  {
    struct timespec ts = ToPosixTimespec(TimeMillis(1000000000001LL));
    EXPECT_EQ(1000000000, ts.tv_sec);
    EXPECT_EQ(1000000, ts.tv_nsec);
  }
}

TEST(TimeTest, GetTime) {
  const auto start_time = GetWallTimeNs();
  const auto start_cputime = GetThreadCPUTimeNs();

  const unsigned ns_in_ms = 1000000;

  for (;;) {
    auto cur_time = GetWallTimeNs();
    auto elapsed = cur_time - start_time;
    // Spin for a little while.
    if (elapsed > TimeNanos(20 * ns_in_ms))
      break;
  }

  auto end_cputime = GetThreadCPUTimeNs();
  auto elapsed_cputime = end_cputime - start_cputime;
  // Check that we're not burning much more CPU time than the length of time
  // that we spun in the loop. We may burn much less, depending on what else is
  // happening on the test machine.
  EXPECT_LE(elapsed_cputime.count(), 50 * ns_in_ms);
}

// This test can work only on Posix platforms which respect the TZ env var.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
TEST(TimeTest, GetTimezoneOffsetMins) {
  const char* tz = getenv("TZ");
  std::string tz_save(tz ? tz : "");
  auto reset_tz_on_exit = OnScopeExit([&] {
    if (!tz_save.empty())
      base::SetEnv("TZ", tz_save.c_str());
  });

  // Note: the sign is reversed in the semantic of the TZ env var.
  // UTC+2 means "2 hours to reach UTC", not "2 hours ahead of UTC".

  base::SetEnv("TZ", "UTC+2");
  EXPECT_EQ(GetTimezoneOffsetMins(), -2 * 60);

  base::SetEnv("TZ", "UTC-2");
  EXPECT_EQ(GetTimezoneOffsetMins(), 2 * 60);

  base::SetEnv("TZ", "UTC-07:45");
  EXPECT_EQ(GetTimezoneOffsetMins(), 7 * 60 + 45);
}
#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
