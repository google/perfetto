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

#ifndef SRC_TRACE_PROCESSOR_UTIL_CONCURRENCY_STRESS_TEST_UTIL_H_
#define SRC_TRACE_PROCESSOR_UTIL_CONCURRENCY_STRESS_TEST_UTIL_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor {

// Spawns `thread_count` threads which each call `fn(thread_idx)` `iterations`
// times, then joins them. Threads block on a start barrier until all have been
// spawned, so the work overlaps as much as possible and races get a chance to
// surface under ThreadSanitizer.
inline void ConcurrentlyRun(
    uint32_t thread_count,
    uint32_t iterations,
    const std::function<void(uint32_t thread_idx)>& fn) {
  PERFETTO_CHECK(thread_count > 0);

  std::mutex mutex;
  std::condition_variable cv;
  uint32_t pending = thread_count;

  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (uint32_t i = 0; i < thread_count; ++i) {
    threads.emplace_back([&, i] {
      {
        std::unique_lock<std::mutex> lock(mutex);
        if (--pending == 0) {
          cv.notify_all();
        } else {
          cv.wait(lock, [&] { return pending == 0; });
        }
      }
      for (uint32_t iter = 0; iter < iterations; ++iter)
        fn(i);
    });
  }

  for (std::thread& thread : threads)
    thread.join();
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_UTIL_CONCURRENCY_STRESS_TEST_UTIL_H_
