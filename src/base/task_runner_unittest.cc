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

#include <random>
#include <thread>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/event_fd.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/lock_free_task_runner.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/waitable_event.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {

template <typename TaskRunnerType>
class TaskRunnerTest : public ::testing::Test {
 public:
  TaskRunnerType task_runner;
};
using LockFreeTaskRunnerTest = TaskRunnerTest<LockFreeTaskRunner>;

namespace {

struct TaskRunnerTestNames {
  template <typename T>
  static std::string GetName(int) {
    if (std::is_same<T, UnixTaskRunner>::value)
      return "UnixTaskRunner";
    if (std::is_same<T, LockFreeTaskRunner>::value)
      return "LockFreeTaskRunner";
    return testing::internal::GetTypeName<T>();
  }
};

using TaskRunnerTypes = ::testing::Types<UnixTaskRunner, LockFreeTaskRunner>;
TYPED_TEST_SUITE(TaskRunnerTest, TaskRunnerTypes, TaskRunnerTestNames);

TYPED_TEST(TaskRunnerTest, QuitImmediately) {
  this->task_runner.PostTask([&] { this->task_runner.Quit(); });
  this->task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, OneTaskFromAnotherThread) {
  WaitableEvent task_runner_started;
  std::thread t1([&] {
    task_runner_started.Wait();
    this->task_runner.PostTask([&] { this->task_runner.Quit(); });
  });
  this->task_runner.PostTask([&] { task_runner_started.Notify(); });
  this->task_runner.Run();
  t1.join();
}

TYPED_TEST(TaskRunnerTest, PostTaskSimple) {
  std::string str;
  this->task_runner.PostTask([&str] { str.append("a"); });
  this->task_runner.PostTask([&str] { str.append("b"); });
  this->task_runner.PostTask([&str] { str.append("c"); });
  this->task_runner.PostTask([&str, tr = &this->task_runner] {
    tr->PostTask([&str] { str.append("d"); });
    tr->PostTask([&str] { str.append("e"); });
    tr->PostTask([&str] { str.append("f"); });
    tr->PostTask([tr] { tr->Quit(); });
  });
  this->task_runner.Run();
  EXPECT_EQ(str, "abcdef");
}

TYPED_TEST(TaskRunnerTest, ManyTasksPostedBeforeRun) {
  constexpr size_t kNumTasks = 10000;
  std::function<void()> post_another;
  size_t last_task_id = 0;
  auto task = [&](size_t n) {
    ASSERT_EQ(last_task_id, n - 1);
    last_task_id = n;
    if (n == kNumTasks)
      this->task_runner.Quit();
  };

  for (size_t i = 1; i <= kNumTasks; i++) {
    this->task_runner.PostTask(std::bind(task, i));
  }

  this->task_runner.Run();
  EXPECT_EQ(last_task_id, kNumTasks);
}

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
  std::vector<int> executed_tasks;
  this->task_runner.PostDelayedTask(
      [&] {
        executed_tasks.push_back(5);
        this->task_runner.Quit();
      },
      100);
  this->task_runner.PostDelayedTask([&] { executed_tasks.push_back(2); }, 20);
  this->task_runner.PostDelayedTask([&] { executed_tasks.push_back(3); }, 20);
  this->task_runner.PostDelayedTask([&] { executed_tasks.push_back(4); }, 80);
  this->task_runner.PostDelayedTask([&] { executed_tasks.push_back(1); }, 10);
  this->task_runner.PostTask([&] {
    this->task_runner.AdvanceTimeForTesting(10);  // Executes task 1.
  });
  this->task_runner.PostTask([&] {
    this->task_runner.AdvanceTimeForTesting(10);  // Executes tasks 2 and 3.
  });
  this->task_runner.PostTask([&] {
    this->task_runner.AdvanceTimeForTesting(60);  // Executes task 4.
  });
  this->task_runner.PostTask([&] {
    this->task_runner.AdvanceTimeForTesting(20);  // Executes task 5.
  });
  this->task_runner.Run();

  EXPECT_THAT(executed_tasks, ::testing::ElementsAre(1, 2, 3, 4, 5));
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
  EventFd evt;
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&task_runner] { task_runner.Quit(); });
  evt.Notify();
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatch) {
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

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatchFromTask) {
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

TYPED_TEST(TaskRunnerTest, AddFileDescriptorWatchFromAnotherWatch) {
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

TYPED_TEST(TaskRunnerTest, RemoveFileDescriptorWatchFromAnotherWatch) {
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

TYPED_TEST(TaskRunnerTest, ReplaceFileDescriptorWatchFromAnotherWatch) {
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

TYPED_TEST(TaskRunnerTest, AddFileDescriptorWatchFromAnotherThread) {
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

TYPED_TEST(TaskRunnerTest, FileDescriptorWatchWithMultipleEvents) {
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

template <typename T>
void RepeatingTask(T* task_runner) {
  task_runner->PostTask(std::bind(&RepeatingTask<T>, task_runner));
}

TYPED_TEST(TaskRunnerTest, FileDescriptorWatchesNotStarved) {
  auto& task_runner = this->task_runner;
  EventFd evt;
  evt.Notify();

  task_runner.PostTask(std::bind(&RepeatingTask<TypeParam>, &task_runner));
  task_runner.AddFileDescriptorWatch(evt.fd(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

template <typename T>
void CountdownTask(T* task_runner, int* counter) {
  if (!--(*counter)) {
    task_runner->Quit();
    return;
  }
  task_runner->PostDelayedTask(
      std::bind(&CountdownTask<T>, task_runner, counter), 1);
}

TYPED_TEST(TaskRunnerTest, NoDuplicateFileDescriptorWatchCallbacks) {
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
  task_runner.PostTask(
      std::bind(&CountdownTask<TypeParam>, &task_runner, &counter));
  task_runner.Run();
}

TYPED_TEST(TaskRunnerTest, ReplaceFileDescriptorWatchFromOtherThread) {
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

TYPED_TEST(TaskRunnerTest, IsIdleForTesting) {
  auto& task_runner = this->task_runner;
  // This first task fails because by the time we get to Run(), there is another
  // one (below) queued up already.
  task_runner.PostTask(
      [&task_runner] { EXPECT_FALSE(task_runner.IsIdleForTesting()); });

  // This one succeeds because it's the last one and there is no further task.
  task_runner.PostTask([&task_runner] {
    EXPECT_TRUE(task_runner.IsIdleForTesting());
    task_runner.Quit();
  });
  task_runner.Run();
}

// Covers a corner cases that TestTaskRunner::RunUntilIdle relies on:
// IsIdleForTesting() is supposed to check for all type of upcoming tasks,
// including FD watches. This is to check that the TaskRunner implementation
// doesn't have off-by one behaviours where the FD watch is only observed on
// the next task.
// It's debatable on whether we need to preserve this behaviour in production
// code, if we assume FDs are unpredictable events and we shouldn't expect
// timing correlations with current tasks.
TYPED_TEST(TaskRunnerTest, IsIdleForTesting_WithFd) {
  auto& task_runner = this->task_runner;
  EventFd efd;
  bool efd_observed = false;

  // This will fail the IsIdleForTesting() check because by the time we get
  // to run, the eventfd is notified.
  task_runner.PostTask(
      [&task_runner] { EXPECT_FALSE(task_runner.IsIdleForTesting()); });

  task_runner.AddFileDescriptorWatch(efd.fd(), [&] {
    efd.Clear();
    efd_observed = true;
    task_runner.PostTask([&task_runner] {
      EXPECT_TRUE(task_runner.IsIdleForTesting());
      task_runner.Quit();
    });
  });
  efd.Notify();

  task_runner.Run();
  EXPECT_TRUE(efd_observed);
}

TYPED_TEST(TaskRunnerTest, RunsTasksOnCurrentThread) {
  auto& main_tr = this->task_runner;

  EXPECT_TRUE(main_tr.RunsTasksOnCurrentThread());
  std::thread thread([&main_tr] {
    TypeParam second_tr;
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

TYPED_TEST(TaskRunnerTest, FileDescriptorWatchFairness) {
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
TYPED_TEST(TaskRunnerTest, FileDescriptorClosedEvent) {
  auto& task_runner = this->task_runner;
  Pipe pipe = Pipe::Create();
  pipe.wr.reset();
  task_runner.AddFileDescriptorWatch(pipe.rd.get(),
                                     [&task_runner] { task_runner.Quit(); });
  task_runner.Run();
}

#endif

TYPED_TEST(TaskRunnerTest, MultiThreadedStress) {
  constexpr size_t kNumThreads = 4;
  constexpr size_t kNumTasksPerThread = 1000;
  constexpr size_t kTotalTasks = kNumThreads * kNumTasksPerThread;
  std::atomic<size_t> tasks_posted{};

  std::array<size_t, kNumThreads> last_task_received{};
  auto task_fn = [&](size_t thread_id, size_t task_num) {
    ASSERT_EQ(last_task_received[thread_id], task_num);
    ++last_task_received[thread_id];
  };

  auto thread_fn = [&](size_t thread_id) {
    std::minstd_rand0 rnd{};
    size_t task_seq = 0;
    for (;;) {
      int num_subtasks = std::uniform_int_distribution<int>(1, 32)(rnd);
      for (int i = 0; i < num_subtasks; ++i) {
        this->task_runner.PostTask(std::bind(task_fn, thread_id, task_seq));
        if (tasks_posted.fetch_add(1, std::memory_order_relaxed) ==
            kTotalTasks - 1) {
          this->task_runner.PostTask([&] { this->task_runner.Quit(); });
        }
        if (++task_seq >= kNumTasksPerThread) {
          return;
        }
      }
      std::this_thread::yield();
    }
  };

  std::array<std::thread, kNumThreads> threads{};
  for (size_t i = 0; i < kNumThreads; ++i) {
    threads[i] = std::thread(std::bind(thread_fn, i));
  }

  this->task_runner.Run();

  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(tasks_posted.load(), kTotalTasks);
}

// [LockFreeTaskRunner-only] Covers the slab allocator logic, ensuring that
// slabs are recycled properly and are not leaked. It run tasks in bursts
// (one tasks spwaning up to kBurstMax subtasks), catches up, then repeats.
TEST_F(LockFreeTaskRunnerTest, NoSlabLeaks) {
  constexpr size_t kMaxTasks = 10000;
  constexpr size_t kBurstMax = task_runner_internal::kSlabSize - 2;
  size_t tasks_posted = 0;
  std::function<void()> task_fn;
  std::minstd_rand0 rnd;
  LockFreeTaskRunner task_runner;

  task_fn = [&] {
    int burst_count = std::uniform_int_distribution<int>(1, kBurstMax)(rnd);
    for (int i = 0; i < burst_count; i++, tasks_posted++) {
      task_runner.PostTask([] {});
    }
    if (tasks_posted < kMaxTasks) {
      task_runner.PostTask(task_fn);
    } else {
      task_runner.PostTask([&] { task_runner.Quit(); });
    }
  };

  task_fn();
  task_runner.Run();

  EXPECT_LE(task_runner.slabs_allocated(), 2u);
}

TYPED_TEST(TaskRunnerTest, RaceOnQuit) {
  std::atomic<LockFreeTaskRunner*> task_runnner{};

  std::thread thread([&]() {
    LockFreeTaskRunner tr;
    std::function<void()> keep_tr_pumped;
    keep_tr_pumped = [&] { tr.PostTask(keep_tr_pumped); };
    tr.PostTask([&] { task_runnner.store(&tr); });
    tr.PostTask(keep_tr_pumped);
    tr.Run();
  });

  LockFreeTaskRunner* tr = nullptr;
  for (; !tr; tr = task_runnner.load()) {
    std::this_thread::yield();
  }

  tr->Quit();
  thread.join();
}

TEST_F(LockFreeTaskRunnerTest, HashSpreading) {
  constexpr uint32_t kBuckets = task_runner_internal::kNumRefcountBuckets;
  constexpr uint32_t kSamples = kBuckets * 16;
  std::array<int, kBuckets> hits{};
  std::vector<std::unique_ptr<task_runner_internal::Slab>> slabs;

  for (uint32_t i = 0; i < kSamples; i++) {
    slabs.emplace_back(std::make_unique<task_runner_internal::Slab>());
    ++hits[task_runner_internal::HashSlabPtr(slabs.back().get())];
  }

  // Print a histogram of the distribution.
  std::string distrib_str = "Hash distribution:\n";
  for (uint32_t i = 0; i < kBuckets; i++) {
    distrib_str += "Bucket " + std::to_string(i) + ": [" +
                   std::to_string(hits[i]) + "]\t" +
                   std::string(static_cast<size_t>(hits[i]), '*') + "\n";
  }
  PERFETTO_DLOG("%s", distrib_str.c_str());

  // Check that the distribution is reasonable.
  // 1. Check that not too many buckets are empty.
  // 2. Check that there are no major hotspots.
  int empty_buckets = 0;
  int max_hits = 0;
  for (int h : hits) {
    if (h == 0)
      empty_buckets++;
    if (h > max_hits)
      max_hits = h;
  }

  // With kSamples, we expect an average of kSamples / kBuckets = 16 hits per
  // bucket. A real random distribution will have some empty buckets.
  // Allow up to 4 empty buckets (12.5%).
  EXPECT_LE(empty_buckets, 4);

  // Check for hotspots. No bucket should have more than 2.5x the average.
  EXPECT_LE(max_hits, kSamples * 2.5 / kBuckets);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
