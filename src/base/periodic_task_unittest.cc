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

#include "perfetto/ext/base/periodic_task.h"

#include "perfetto/ext/base/file_utils.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace perfetto {
namespace base {

namespace {

TEST(PeriodicTaskTest, PostDelayedTaskMode) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks = 0;
  auto quit_closure = task_runner.CreateCheckpoint("all_timers_done");

  PeriodicTask::Args args;
  args.task = [&] {
    if (++num_callbacks == 3)
      quit_closure();
  };
  args.period_ms = 1;
  args.start_first_task_immediately = true;
  pt.Start(std::move(args));
  EXPECT_EQ(num_callbacks, 1u);
  task_runner.RunUntilCheckpoint("all_timers_done");
  EXPECT_EQ(num_callbacks, 3u);
}

TEST(PeriodicTaskTest, OneShot) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks = 0;
  auto quit_closure = task_runner.CreateCheckpoint("one_shot_done");

  PeriodicTask::Args args;
  args.use_suspend_aware_timer = true;
  args.one_shot = true;
  args.period_ms = 1;
  args.task = [&] {
    ASSERT_EQ(++num_callbacks, 1u);
    quit_closure();
  };
  pt.Start(std::move(args));
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  task_runner.RunUntilCheckpoint("one_shot_done");
  EXPECT_EQ(num_callbacks, 1u);
}

// Call Reset() from a callback, ensure no further calls are made.
TEST(PeriodicTaskTest, ResetFromCallback) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks = 0;
  PeriodicTask::Args args;
  auto quit_closure = task_runner.CreateCheckpoint("quit_closure");
  args.task = [&] {
    ++num_callbacks;
    pt.Reset();
    task_runner.PostDelayedTask(quit_closure, 5);
  };
  args.period_ms = 1;
  pt.Start(std::move(args));
  EXPECT_EQ(num_callbacks, 0u);  // No immediate execution.

  task_runner.RunUntilCheckpoint("quit_closure");
  EXPECT_EQ(num_callbacks, 1u);
}

// Invalidates the timerfd, by replacing it with /dev/null, in the middle of
// the periodic ticks. That causes the next read() to fail and fall back on
// PostDelayedTask().
// On Mac and other systems where timerfd is not supported this will fall back
// on PostDelayedTask() immediately (and work).
TEST(PeriodicTaskTest, FallbackIfTimerfdFails) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks = 0;
  auto quit_closure = task_runner.CreateCheckpoint("all_timers_done");

  PeriodicTask::Args args;
  args.task = [&] {
    ++num_callbacks;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    if (num_callbacks == 3 && pt.timer_fd_for_testing() > 0) {
      ScopedFile dev_null = OpenFile("/dev/null", O_RDONLY);
      dup2(*dev_null, pt.timer_fd_for_testing());
    }
#else
    EXPECT_FALSE(base::ScopedPlatformHandle::ValidityChecker::IsValid(
        pt.timer_fd_for_testing()));
#endif
    if (num_callbacks == 6)
      quit_closure();
  };
  args.period_ms = 1;
  args.use_suspend_aware_timer = true;
  pt.Start(std::move(args));
  task_runner.RunUntilCheckpoint("all_timers_done");
  EXPECT_EQ(num_callbacks, 6u);
}

TEST(PeriodicTaskTest, DestroyedFromCallback) {
  TestTaskRunner task_runner;
  std::unique_ptr<PeriodicTask> pt(new PeriodicTask(&task_runner));
  uint32_t num_callbacks = 0;
  PeriodicTask::Args args;
  auto quit_closure = task_runner.CreateCheckpoint("quit_closure");
  args.task = [&] {
    ++num_callbacks;
    pt.reset();
    task_runner.PostDelayedTask(quit_closure, 5);
  };
  args.period_ms = 1;
  args.use_suspend_aware_timer = true;
  pt->Start(std::move(args));

  task_runner.RunUntilCheckpoint("quit_closure");
  EXPECT_EQ(num_callbacks, 1u);
  EXPECT_FALSE(pt);
}

TEST(PeriodicTaskTest, DestroyedFromAnotherTask) {
  TestTaskRunner task_runner;
  std::unique_ptr<PeriodicTask> pt(new PeriodicTask(&task_runner));
  uint32_t num_callbacks = 0;
  PeriodicTask::Args args;
  auto quit_closure = task_runner.CreateCheckpoint("quit_closure");
  args.task = [&] {
    if (++num_callbacks == 2) {
      task_runner.PostTask([&] {
        pt.reset();
        task_runner.PostDelayedTask(quit_closure, 5);
      });
    }
  };
  args.period_ms = 1;
  args.use_suspend_aware_timer = true;
  pt->Start(std::move(args));

  task_runner.RunUntilCheckpoint("quit_closure");
  EXPECT_EQ(num_callbacks, 2u);
  EXPECT_FALSE(pt);
}

// Checks the generation logic.
TEST(PeriodicTaskTest, RestartWhileRunning) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks_a = 0;
  uint32_t num_callbacks_b = 0;
  auto quit_closure = task_runner.CreateCheckpoint("quit_closure");

  auto reuse = [&] {
    PeriodicTask::Args args;
    args.period_ms = 1;
    args.task = [&] {
      if (++num_callbacks_b == 3)
        quit_closure();
    };
    pt.Start(std::move(args));
  };

  PeriodicTask::Args args;
  args.task = [&] {
    if (++num_callbacks_a == 2)
      task_runner.PostTask(reuse);
  };
  args.period_ms = 1;
  args.use_suspend_aware_timer = true;
  pt.Start(std::move(args));

  task_runner.RunUntilCheckpoint("quit_closure");
  EXPECT_EQ(num_callbacks_a, 2u);
  EXPECT_EQ(num_callbacks_b, 3u);
}

TEST(PeriodicTaskTest, ImmediateExecution) {
  TestTaskRunner task_runner;
  PeriodicTask pt(&task_runner);
  uint32_t num_callbacks = 0;

  PeriodicTask::Args args;
  args.task = [&] { ++num_callbacks; };
  args.period_ms = 1;
  pt.Start(args);
  EXPECT_EQ(num_callbacks, 0u);  // No immediate execution.

  args.start_first_task_immediately = true;
  pt.Start(args);
  EXPECT_EQ(num_callbacks, 1u);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
