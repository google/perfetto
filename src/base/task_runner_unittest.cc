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

#include "perfetto/base/build_config.h"

#include "perfetto/ext/base/unix_task_runner.h"

#include <thread>

#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

class TaskRunnerTest : public ::testing::Test {
 public:
  UnixTaskRunner task_runner;
};

TEST_F(TaskRunnerTest, PostImmediateTask) {
  auto& task_runner = this->task_runner;
  int counter = 0;
  task_runner.PostTask([&counter] { counter = (counter << 4) | 1; });
  task_runner.PostTask([&counter] { counter = (counter << 4) | 2; });
  task_runner.PostTask([&counter] { counter = (counter << 4) | 3; });
  task_runner.PostTask([&counter] { counter = (counter << 4) | 4; });
  task_runner.PostTask([&task_runner] { task_runner.Quit(); });
  task_runner.Run();
  EXPECT_EQ(0x1234, counter);
}

TEST_F(TaskRunnerTest, PostDelayedTask) {
  auto& task_runner = this->task_runner;
  int counter = 0;
  task_runner.PostDelayedTask([&counter] { counter = (counter << 4) | 1; }, 5);
  task_runner.PostDelayedTask([&counter] { counter = (counter << 4) | 2; }, 10);
  task_runner.PostDelayedTask([&counter] { counter = (counter << 4) | 3; }, 15);
  task_runner.PostDelayedTask([&counter] { counter = (counter << 4) | 4; }, 15);
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 20);
  task_runner.Run();
  EXPECT_EQ(0x1234, counter);
}

TEST_F(TaskRunnerTest, PostImmediateTaskFromTask) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask([&task_runner] {
    task_runner.PostTask([&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
}

TEST_F(TaskRunnerTest, PostDelayedTaskFromTask) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask([&task_runner] {
    task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  });
  task_runner.Run();
}

TEST_F(TaskRunnerTest, PostImmediateTaskFromOtherThread) {
  auto& task_runner = this->task_runner;
  ThreadChecker thread_checker;
  int counter = 0;
  std::thread thread([&task_runner, &counter, &thread_checker] {
    task_runner.PostTask([&thread_checker] {
      EXPECT_TRUE(thread_checker.CalledOnValidThread());
    });
    task_runner.PostTask([&counter] { counter = (counter << 4) | 1; });
    task_runner.PostTask([&counter] { counter = (counter << 4) | 2; });
    task_runner.PostTask([&counter] { counter = (counter << 4) | 3; });
    task_runner.PostTask([&counter] { counter = (counter << 4) | 4; });
    task_runner.PostTask([&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
  thread.join();
  EXPECT_EQ(0x1234, counter);
}

TEST_F(TaskRunnerTest, PostDelayedTaskFromOtherThread) {
  auto& task_runner = this->task_runner;
  std::thread thread([&task_runner] {
    task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  });
  task_runner.Run();
  thread.join();
}

TEST_F(TaskRunnerTest, AddFileDescriptorWatch) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&task_runner] { task_runner.Quit(); });
  evt.Notify();
  task_runner.Run();
}

TEST_F(TaskRunnerTest, RemoveFileDescriptorWatch) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  bool watch_ran = false;
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.RemoveFileDescriptorWatch(evt.fd());
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TEST_F(TaskRunnerTest, RemoveFileDescriptorWatchFromTask) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  bool watch_ran = false;
  task_runner.PostTask([&task_runner, &evt] {
    task_runner.RemoveFileDescriptorWatch(evt.fd());
  });
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TEST_F(TaskRunnerTest, AddFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  EventFd evt2;
  evt.Notify();
  evt2.Notify();
  task_runner.AddFileDescriptorWatch(evt.fd(), [&task_runner, &evt, &evt2] {
    evt.Clear();
    task_runner.AddFileDescriptorWatch(evt2.fd(),
                                       [&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
}

TEST_F(TaskRunnerTest, RemoveFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  EventFd evt2;
  evt.Notify();

  bool watch_ran = false;
  task_runner.AddFileDescriptorWatch(evt.fd(), [&task_runner, &evt, &evt2] {
    evt.Clear();
    evt2.Notify();
    task_runner.RemoveFileDescriptorWatch(evt2.fd());
  });
  task_runner.AddFileDescriptorWatch(evt2.fd(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TEST_F(TaskRunnerTest, ReplaceFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  EventFd evt2;

  bool watch_ran = false;
  evt.Notify();
  task_runner.AddFileDescriptorWatch(evt.fd(), [&task_runner, &evt, &evt2] {
    evt.Clear();
    evt2.Notify();
    task_runner.RemoveFileDescriptorWatch(evt2.fd());
    task_runner.AddFileDescriptorWatch(evt2.fd(),
                                       [&task_runner] { task_runner.Quit(); });
  });
  task_runner.AddFileDescriptorWatch(evt2.fd(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TEST_F(TaskRunnerTest, AddFileDescriptorWatchFromAnotherThread) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  std::thread thread([&task_runner, &evt] {
    task_runner.AddFileDescriptorWatch(evt.fd(),
                                       [&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
  thread.join();
}

TEST_F(TaskRunnerTest, FileDescriptorWatchWithMultipleEvents) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  int event_count = 0;
  task_runner.AddFileDescriptorWatch(
      evt.fd(), [&task_runner, &evt, &event_count] {
        ASSERT_LT(event_count, 3);
        if (++event_count == 3) {
          task_runner.Quit();
          return;
        }
        evt.Clear();
        task_runner.PostTask([&evt] { evt.Notify(); });
      });
  task_runner.Run();
}

TEST_F(TaskRunnerTest, PostManyDelayedTasks) {
  // Check that PostTask doesn't start failing if there are too many scheduled
  // wake-ups.
  auto& task_runner = this->task_runner;
  for (int i = 0; i < 0x1000; i++)
    task_runner.PostDelayedTask([] {}, 0);
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();
}

TEST_F(TaskRunnerTest, RunAgain) {
  auto& task_runner = this->task_runner;
  int counter = 0;
  task_runner.PostTask([&task_runner, &counter] {
    counter++;
    task_runner.Quit();
  });
  task_runner.Run();
  task_runner.PostTask([&task_runner, &counter] {
    counter++;
    task_runner.Quit();
  });
  task_runner.Run();
  EXPECT_EQ(2, counter);
}

void RepeatingTask(UnixTaskRunner* task_runner) {
  task_runner->PostTask(std::bind(&RepeatingTask, task_runner));
}

TEST_F(TaskRunnerTest, FileDescriptorWatchesNotStarved) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  task_runner.PostTask(std::bind(&RepeatingTask, &task_runner));
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

void CountdownTask(UnixTaskRunner* task_runner, int* counter) {
  if (!--(*counter)) {
    task_runner->Quit();
    return;
  }
  task_runner->PostDelayedTask(std::bind(&CountdownTask, task_runner, counter),
                               1);
}

TEST_F(TaskRunnerTest, NoDuplicateFileDescriptorWatchCallbacks) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  bool watch_called = 0;
  int counter = 10;
  task_runner.AddFileDescriptorWatch(evt.fd(), [&evt, &watch_called] {
    ASSERT_FALSE(watch_called);
    evt.Clear();
    watch_called = true;
  });
  task_runner.PostTask(std::bind(&CountdownTask, &task_runner, &counter));
  task_runner.Run();
}

TEST_F(TaskRunnerTest, ReplaceFileDescriptorWatchFromOtherThread) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  // The two watch tasks here race each other. We don't particularly care which
  // wins as long as one of them runs.
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&task_runner] { task_runner.Quit(); });

  std::thread thread([&task_runner, &evt] {
    task_runner.RemoveFileDescriptorWatch(evt.fd());
    task_runner.AddFileDescriptorWatch(evt.fd(),
                                       [&task_runner] { task_runner.Quit(); });
  });

  task_runner.Run();
  thread.join();
}

TEST_F(TaskRunnerTest, IsIdleForTesting) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask(
      [&task_runner] { EXPECT_FALSE(task_runner.IsIdleForTesting()); });
  task_runner.PostTask([&task_runner] {
    EXPECT_TRUE(task_runner.IsIdleForTesting());
    task_runner.Quit();
  });
  task_runner.Run();
}

TEST_F(TaskRunnerTest, RunsTasksOnCurrentThread) {
  auto& main_tr = this->task_runner;

  EXPECT_TRUE(main_tr.RunsTasksOnCurrentThread());
  std::thread thread([&main_tr] {
    typename std::remove_reference<decltype(main_tr)>::type second_tr;
    second_tr.PostTask([&main_tr, &second_tr] {
      EXPECT_FALSE(main_tr.RunsTasksOnCurrentThread());
      EXPECT_TRUE(second_tr.RunsTasksOnCurrentThread());
      second_tr.Quit();
    });
    second_tr.Run();
  });
  main_tr.PostTask([&]() { main_tr.Quit(); });
  main_tr.Run();
  thread.join();
}

TEST_F(TaskRunnerTest, FileDescriptorWatchFairness) {
  auto& task_runner = this->task_runner;
  EventFd evt[5];
  std::map<PlatformHandle, int /*num_tasks*/> num_tasks;
  static constexpr int kNumTasksPerHandle = 100;
  for (auto& e : evt) {
    e.Notify();
    task_runner.AddFileDescriptorWatch(e.fd(), [&] {
      if (++num_tasks[e.fd()] == kNumTasksPerHandle) {
        e.Clear();
        task_runner.Quit();
      }
    });
  }

  task_runner.Run();

  // The sequence evt[0], evt[1], evt[2] should be repeated N times. On the
  // Nth time the task runner quits. All tasks should have been running at least
  // N-1 times (we can't predict which one of the tasks will quit).
  for (auto& e : evt) {
    ASSERT_GE(num_tasks[e.fd()], kNumTasksPerHandle - 1);
    ASSERT_LE(num_tasks[e.fd()], kNumTasksPerHandle);
  }
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

// This tests UNIX-specific behavior on pipe closure.
TEST_F(TaskRunnerTest, FileDescriptorClosedEvent) {
  auto& task_runner = this->task_runner;
  Pipe pipe = Pipe::Create();
  pipe.wr.reset();
  task_runner.AddFileDescriptorWatch(pipe.rd.get(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
