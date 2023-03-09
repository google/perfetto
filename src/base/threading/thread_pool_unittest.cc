/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "perfetto/ext/base//threading/thread_pool.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "perfetto/ext/base/waitable_event.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

struct ThreadLatch {
  base::WaitableEvent notify;
  base::WaitableEvent wait;
  bool task_started = false;
};

TEST(ThreadPoolTest, SequentialQueueing) {
  ThreadLatch first;
  ThreadLatch second;
  base::ThreadPool pool(1);

  pool.PostTask([&first] {
    first.task_started = true;
    first.notify.Notify();
    first.wait.Wait();
  });

  pool.PostTask([&second] {
    second.task_started = true;
    second.notify.Notify();
    second.wait.Wait();
  });

  first.notify.Wait();
  ASSERT_TRUE(first.task_started);
  ASSERT_FALSE(second.task_started);
  first.wait.Notify();

  second.notify.Wait();
  ASSERT_TRUE(second.task_started);
  second.wait.Notify();
}

TEST(ThreadPoolTest, ParallelSecondFinishFirst) {
  base::ThreadPool pool(2);

  ThreadLatch first;
  pool.PostTask([&first] {
    first.wait.Wait();
    first.task_started = true;
    first.notify.Notify();
  });

  ThreadLatch second;
  pool.PostTask([&second] {
    second.wait.Wait();
    second.task_started = true;
    second.notify.Notify();
  });

  second.wait.Notify();
  second.notify.Wait();
  ASSERT_TRUE(second.task_started);

  first.wait.Notify();
  first.notify.Wait();
  ASSERT_TRUE(first.task_started);
}

TEST(ThreadPoolTest, StressTest) {
  std::mutex mu;
  std::condition_variable cv;
  uint32_t count = 0;
  base::ThreadPool pool(128);
  for (uint32_t i = 0; i < 1024; ++i) {
    pool.PostTask([&mu, &count, &cv] {
      std::lock_guard<std::mutex> guard(mu);
      if (++count == 1024) {
        cv.notify_one();
      }
    });
  }

  std::unique_lock<std::mutex> lock(mu);
  cv.wait(lock, [&count]() { return count == 1024u; });
}

}  // namespace
}  // namespace base
}  // namespace perfetto
