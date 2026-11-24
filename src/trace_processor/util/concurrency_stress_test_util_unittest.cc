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

#include "src/trace_processor/util/concurrency_stress_test_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(ConcurrencyStressTestUtilTest, InvocationCount) {
  std::atomic<uint64_t> counter{0};
  ConcurrentlyRun(8, 1000, [&](uint32_t) { counter.fetch_add(1); });
  EXPECT_EQ(counter.load(), 8u * 1000u);
}

TEST(ConcurrencyStressTestUtilTest, RunsThreadsConcurrently) {
  constexpr uint32_t kThreads = 8;

  // Each thread's first iteration waits until every thread is live. A
  // sequential harness could never have all threads present at once, so the
  // bounded wait would time out and fail the assertion instead of hanging.
  std::atomic<uint32_t> present{0};
  std::atomic<uint32_t> rendezvous_reached{0};
  std::atomic<bool> first[kThreads];
  for (uint32_t i = 0; i < kThreads; ++i)
    first[i].store(true);

  ConcurrentlyRun(kThreads, 5, [&](uint32_t thread_idx) {
    if (!first[thread_idx].exchange(false))
      return;
    present.fetch_add(1);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (present.load() < kThreads) {
      if (std::chrono::steady_clock::now() > deadline)
        return;
      std::this_thread::yield();
    }
    rendezvous_reached.fetch_add(1);
  });

  EXPECT_EQ(rendezvous_reached.load(), kThreads)
      << "threads did not all run concurrently";
}

// Deliberately racy: shows the harness surfaces a data race under
// ThreadSanitizer. Disabled so it never runs in CI.
TEST(ConcurrencyStressTestUtilTest, DISABLED_DetectsRaces) {
  int unguarded = 0;
  ConcurrentlyRun(8, 1000, [&](uint32_t) { unguarded++; });
  EXPECT_GE(unguarded, 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
