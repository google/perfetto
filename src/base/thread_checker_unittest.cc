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

#include "perfetto/ext/base/thread_checker.h"

#include <functional>
#include <memory>
#include <thread>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

bool RunOnThread(std::function<bool(void)> closure) {
  bool res = false;
  std::thread thread([&res, &closure] { res = closure(); });
  thread.join();
  return res;
}

TEST(ThreadCheckerTest, Basic) {
  ThreadChecker thread_checker;
  ASSERT_TRUE(thread_checker.CalledOnValidThread());
  bool res = RunOnThread(
      [&thread_checker] { return thread_checker.CalledOnValidThread(); });
  ASSERT_TRUE(thread_checker.CalledOnValidThread());
  ASSERT_FALSE(res);
}

TEST(ThreadCheckerTest, Detach) {
  ThreadChecker thread_checker;
  ASSERT_TRUE(thread_checker.CalledOnValidThread());
  thread_checker.DetachFromThread();
  bool res = RunOnThread(
      [&thread_checker] { return thread_checker.CalledOnValidThread(); });
  ASSERT_TRUE(res);
  ASSERT_FALSE(thread_checker.CalledOnValidThread());
}

TEST(ThreadCheckerTest, CopyConstructor) {
  ThreadChecker thread_checker;
  ThreadChecker copied_thread_checker = thread_checker;
  ASSERT_TRUE(thread_checker.CalledOnValidThread());
  ASSERT_TRUE(copied_thread_checker.CalledOnValidThread());
  bool res = RunOnThread([&copied_thread_checker] {
    return copied_thread_checker.CalledOnValidThread();
  });
  ASSERT_FALSE(res);

  copied_thread_checker.DetachFromThread();
  res = RunOnThread([&thread_checker, &copied_thread_checker] {
    return copied_thread_checker.CalledOnValidThread() &&
           !thread_checker.CalledOnValidThread();
  });
  ASSERT_TRUE(res);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
