/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <pthread.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <condition_variable>
#include <thread>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/base/time.h"
#include "test/task_runner_thread.h"

namespace perfetto {

TaskRunnerThread::TaskRunnerThread(const char* name) : name_(name) {}
TaskRunnerThread::~TaskRunnerThread() {
  Stop();
}

void TaskRunnerThread::Start(std::unique_ptr<ThreadDelegate> delegate) {
  // Begin holding the lock for the condition variable.
  std::unique_lock<std::mutex> lock(mutex_);

  // Start the thread.
  PERFETTO_DCHECK(!runner_);
  thread_ = std::thread(&TaskRunnerThread::Run, this, std::move(delegate));

  // Wait for runner to be ready.
  ready_.wait_for(lock, std::chrono::seconds(10),
                  [this]() { return runner_ != nullptr; });
}

void TaskRunnerThread::Stop() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (runner_)
      runner_->Quit();
  }

  if (thread_.joinable())
    thread_.join();
}

uint64_t TaskRunnerThread::GetThreadCPUTimeNs() {
  std::condition_variable cv;
  std::unique_lock<std::mutex> lock(mutex_);
  uint64_t thread_time_ns = 0;

  if (!runner_)
    return 0;

  runner_->PostTask([this, &thread_time_ns, &cv] {
    std::unique_lock<std::mutex> inner_lock(mutex_);
    thread_time_ns = static_cast<uint64_t>(base::GetThreadCPUTimeNs().count());
    cv.notify_one();
  });

  cv.wait(lock, [&thread_time_ns] { return thread_time_ns != 0; });
  return thread_time_ns;
}

void TaskRunnerThread::Run(std::unique_ptr<ThreadDelegate> delegate) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
  pthread_setname_np(name_);
#else
  pthread_setname_np(pthread_self(), name_);
#endif

  // Create the task runner and execute the specicalised code.
  base::PlatformTaskRunner task_runner;
  delegate->Initialize(&task_runner);

  // Pass the runner back to the main thread.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    runner_ = &task_runner;
  }

  // Notify the main thread that the runner is ready.
  ready_.notify_one();

  // Spin the loop.
  task_runner.Run();

  // Ensure we clear out the delegate before runner goes out
  // of scope.
  delegate.reset();

  // Cleanup the runner.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    runner_ = nullptr;
  }
}

ThreadDelegate::~ThreadDelegate() = default;

}  // namespace perfetto
