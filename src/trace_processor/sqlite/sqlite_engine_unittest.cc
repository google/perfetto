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

#include "src/trace_processor/sqlite/sqlite_engine.h"

#include <sqlite3.h>

#include "perfetto/base/time.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

// The retries themselves spin on |base::GetWallTimeMs|, so to make these
// tests deterministic we install a fake sleep function that does nothing.
// |GivesUpAtTimeout| uses a real but tiny (50ms) timeout so the wall-clock
// loop terminates promptly.
void NoSleep(unsigned /*interval_us*/) {}

TEST(BusyRetryHelperTest, RetriesUntilSuccess) {
  BusyRetryHelper retry;
  retry.set_sleep_fn_for_testing(&NoSleep);

  // Two BUSY responses — both should request a retry.
  EXPECT_TRUE(retry.ShouldRetry(SQLITE_BUSY));
  EXPECT_TRUE(retry.ShouldRetry(SQLITE_BUSY));

  // SQLITE_OK is not a retry trigger; the helper signals "stop".
  EXPECT_FALSE(retry.ShouldRetry(SQLITE_OK));
}

TEST(BusyRetryHelperTest, RetriesOnLocked) {
  BusyRetryHelper retry;
  retry.set_sleep_fn_for_testing(&NoSleep);

  // SQLITE_LOCKED is treated identically to BUSY — same recovery semantics
  // for shared-cache contention.
  EXPECT_TRUE(retry.ShouldRetry(SQLITE_LOCKED));
  EXPECT_TRUE(retry.ShouldRetry(SQLITE_LOCKED));
}

TEST(BusyRetryHelperTest, GivesUpAtTimeout) {
  // 50ms timeout, no real sleeps — the loop should exit once
  // |base::GetWallTimeMs()| crosses the deadline.
  BusyRetryHelper retry(base::TimeMillis(50));
  retry.set_sleep_fn_for_testing(&NoSleep);

  base::TimeMillis start = base::GetWallTimeMs();
  int retries = 0;
  while (retry.ShouldRetry(SQLITE_BUSY)) {
    retries++;
    if (retries > 100000000) {
      FAIL() << "ShouldRetry failed to terminate within timeout";
    }
  }
  base::TimeMillis elapsed = base::GetWallTimeMs() - start;
  // The helper polled |GetWallTimeMs| each iteration; the wall-clock
  // delta should be at least the timeout but bounded loosely to catch
  // a runaway loop.
  EXPECT_GE(elapsed.count(), 50);
  EXPECT_LT(elapsed.count(), 5000);
}

TEST(BusyRetryHelperTest, PassesThroughOtherErrors) {
  BusyRetryHelper retry;
  retry.set_sleep_fn_for_testing(&NoSleep);

  // Real SQLite errors (constraint violation, plain error, etc.) must
  // propagate immediately — no retry, no sleep.
  EXPECT_FALSE(retry.ShouldRetry(SQLITE_ERROR));
  EXPECT_FALSE(retry.ShouldRetry(SQLITE_CONSTRAINT));
  EXPECT_FALSE(retry.ShouldRetry(SQLITE_MISUSE));
}

}  // namespace
}  // namespace perfetto::trace_processor
