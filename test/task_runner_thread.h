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

#ifndef TEST_TASK_RUNNER_THREAD_H_
#define TEST_TASK_RUNNER_THREAD_H_

#include <condition_variable>
#include <mutex>
#include <thread>

#include "perfetto/base/task_runner.h"
#include "src/base/test/test_task_runner.h"

namespace perfetto {

// Used to perform initialization work on a background TaskRunnerThread.
class ThreadDelegate {
 public:
  virtual ~ThreadDelegate();

  // Invoked on the target thread before the message loop is started.
  virtual void Initialize(base::TaskRunner* task_runner) = 0;
};

// Background thread which spins a task runner until completed or the thread is
// destroyed. If the thread is destroyed before the task runner completes, the
// task runner is quit and the thread is joined.
class TaskRunnerThread {
 public:
  explicit TaskRunnerThread(const char* name);
  ~TaskRunnerThread();

  // Blocks until the thread has been created and Initialize() has been
  // called.
  void Start(std::unique_ptr<ThreadDelegate> delegate);

  // Blocks until the thread has been stopped and joined.
  void Stop();

  uint64_t GetThreadCPUTimeNs();

 private:
  void Run(std::unique_ptr<ThreadDelegate> delegate);

  const char* const name_;
  std::thread thread_;
  std::condition_variable ready_;

  // All variables below this point are protected by |mutex_|.
  std::mutex mutex_;
  base::PlatformTaskRunner* runner_ = nullptr;
};

}  // namespace perfetto

#endif  // TEST_TASK_RUNNER_THREAD_H_
