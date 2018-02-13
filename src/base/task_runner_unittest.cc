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

#include "perfetto/base/unix_task_runner.h"

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/scoped_file.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
#include "perfetto/base/android_task_runner.h"
#endif

#include <thread>

namespace perfetto {
namespace base {
namespace {

template <typename T>
class TaskRunnerTest : public ::testing::Test {
 public:
  T task_runner;
};

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    !PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
using TaskRunnerTypes = ::testing::Types<AndroidTaskRunner, UnixTaskRunner>;
#else
using TaskRunnerTypes = ::testing::Types<UnixTaskRunner>;
#endif
TYPED_TEST_CASE(TaskRunnerTest, TaskRunnerTypes);

struct Pipe {
  Pipe() {
    int pipe_fds[2];
    PERFETTO_DCHECK(pipe(pipe_fds) == 0);
    read_fd.reset(pipe_fds[0]);
    write_fd.reset(pipe_fds[1]);
    // Make the pipe initially readable.
    Write();
  }

  void Read() {
    char b;
    PERFETTO_DCHECK(read(read_fd.get(), &b, 1) == 1);
  }

  void Write() {
    const char b = '?';
    PERFETTO_DCHECK(write(write_fd.get(), &b, 1) == 1);
  }

  ScopedFile read_fd;
  ScopedFile write_fd;
};

TYPED_TEST(TaskRunnerTest, PostImmediateTask) {
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

TYPED_TEST(TaskRunnerTest, PostDelayedTask) {
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

TYPED_TEST(TaskRunnerTest, PostImmediateTaskFromTask) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask([&task_runner] {
    task_runner.PostTask([&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, PostDelayedTaskFromTask) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask([&task_runner] {
    task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, PostImmediateTaskFromOtherThread) {
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

TYPED_TEST(TaskRunnerTest, PostDelayedTaskFromOtherThread) {
  auto& task_runner = this->task_runner;
  std::thread thread([&task_runner] {
    task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  });
  task_runner.Run();
  thread.join();
}

TYPED_TEST(TaskRunnerTest, AddFileDescriptorWatch) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatch) {
  auto& task_runner = this->task_runner;
  Pipe pipe;

  bool watch_ran = false;
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.RemoveFileDescriptorWatch(pipe.read_fd.get());
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatchFromTask) {
  auto& task_runner = this->task_runner;
  Pipe pipe;

  bool watch_ran = false;
  task_runner.PostTask([&task_runner, &pipe] {
    task_runner.RemoveFileDescriptorWatch(pipe.read_fd.get());
  });
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TYPED_TEST(TaskRunnerTest, AddFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  Pipe pipe2;

  task_runner.AddFileDescriptorWatch(
      pipe.read_fd.get(), [&task_runner, &pipe, &pipe2] {
        pipe.Read();
        task_runner.AddFileDescriptorWatch(
            pipe2.read_fd.get(), [&task_runner] { task_runner.Quit(); });
      });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  Pipe pipe2;

  bool watch_ran = false;
  task_runner.AddFileDescriptorWatch(
      pipe.read_fd.get(), [&task_runner, &pipe, &pipe2] {
        pipe.Read();
        task_runner.RemoveFileDescriptorWatch(pipe2.read_fd.get());
      });
  task_runner.AddFileDescriptorWatch(pipe2.read_fd.get(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TYPED_TEST(TaskRunnerTest, ReplaceFileDescriptorWatchFromAnotherWatch) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  Pipe pipe2;

  bool watch_ran = false;
  task_runner.AddFileDescriptorWatch(
      pipe.read_fd.get(), [&task_runner, &pipe2] {
        task_runner.RemoveFileDescriptorWatch(pipe2.read_fd.get());
        task_runner.AddFileDescriptorWatch(
            pipe2.read_fd.get(), [&task_runner] { task_runner.Quit(); });
      });
  task_runner.AddFileDescriptorWatch(pipe2.read_fd.get(),
                                     [&watch_ran] { watch_ran = true; });
  task_runner.Run();

  EXPECT_FALSE(watch_ran);
}

TYPED_TEST(TaskRunnerTest, AddFileDescriptorWatchFromAnotherThread) {
  auto& task_runner = this->task_runner;
  Pipe pipe;

  std::thread thread([&task_runner, &pipe] {
    task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                       [&task_runner] { task_runner.Quit(); });
  });
  task_runner.Run();
  thread.join();
}

TYPED_TEST(TaskRunnerTest, FileDescriptorWatchWithMultipleEvents) {
  auto& task_runner = this->task_runner;
  Pipe pipe;

  int event_count = 0;
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&task_runner, &pipe, &event_count] {
                                       if (++event_count == 3) {
                                         task_runner.Quit();
                                         return;
                                       }
                                       pipe.Read();
                                     });
  task_runner.PostTask([&pipe] { pipe.Write(); });
  task_runner.PostTask([&pipe] { pipe.Write(); });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, FileDescriptorClosedEvent) {
  auto& task_runner = this->task_runner;
  int pipe_fds[2];
  PERFETTO_DCHECK(pipe(pipe_fds) == 0);
  ScopedFile read_fd(pipe_fds[0]);
  ScopedFile write_fd(pipe_fds[1]);

  write_fd.reset();
  task_runner.AddFileDescriptorWatch(read_fd.get(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, PostManyDelayedTasks) {
  // Check that PostTask doesn't start failing if there are too many scheduled
  // wake-ups.
  auto& task_runner = this->task_runner;
  for (int i = 0; i < 0x1000; i++)
    task_runner.PostDelayedTask([] {}, 0);
  task_runner.PostDelayedTask([&task_runner] { task_runner.Quit(); }, 10);
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, RunAgain) {
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

template <typename TaskRunner>
void RepeatingTask(TaskRunner* task_runner) {
  task_runner->PostTask(std::bind(&RepeatingTask<TaskRunner>, task_runner));
}

TYPED_TEST(TaskRunnerTest, FileDescriptorWatchesNotStarved) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  task_runner.PostTask(std::bind(&RepeatingTask<TypeParam>, &task_runner));
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

template <typename TaskRunner>
void CountdownTask(TaskRunner* task_runner, int* counter) {
  if (!--(*counter)) {
    task_runner->Quit();
    return;
  }
  task_runner->PostTask(
      std::bind(&CountdownTask<TaskRunner>, task_runner, counter));
}

TYPED_TEST(TaskRunnerTest, NoDuplicateFileDescriptorWatchCallbacks) {
  auto& task_runner = this->task_runner;
  Pipe pipe;
  bool watch_called = 0;
  int counter = 10;
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&pipe, &watch_called] {
                                       ASSERT_FALSE(watch_called);
                                       pipe.Read();
                                       watch_called = true;
                                     });
  task_runner.PostTask(
      std::bind(&CountdownTask<TypeParam>, &task_runner, &counter));
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, ReplaceFileDescriptorWatchFromOtherThread) {
  auto& task_runner = this->task_runner;
  Pipe pipe;

  // The two watch tasks here race each other. We don't particularly care which
  // wins as long as one of them runs.
  task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                     [&task_runner] { task_runner.Quit(); });

  std::thread thread([&task_runner, &pipe] {
    task_runner.RemoveFileDescriptorWatch(pipe.read_fd.get());
    task_runner.AddFileDescriptorWatch(pipe.read_fd.get(),
                                       [&task_runner] { task_runner.Quit(); });
  });

  task_runner.Run();
  thread.join();
}

TYPED_TEST(TaskRunnerTest, IsIdleForTesting) {
  auto& task_runner = this->task_runner;
  task_runner.PostTask(
      [&task_runner] { EXPECT_FALSE(task_runner.IsIdleForTesting()); });
  task_runner.PostTask([&task_runner] {
    EXPECT_TRUE(task_runner.IsIdleForTesting());
    task_runner.Quit();
  });
  task_runner.Run();
}

}  // namespace
}  // namespace base
}  // namespace perfetto
