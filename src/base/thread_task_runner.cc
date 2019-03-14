/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/base/thread_task_runner.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "perfetto/base/logging.h"
#include "perfetto/base/unix_task_runner.h"

namespace perfetto {
namespace base {

ThreadTaskRunner::ThreadTaskRunner(ThreadTaskRunner&& other) noexcept
    : thread_(std::move(other.thread_)), task_runner_(other.task_runner_) {
  other.task_runner_ = nullptr;
}

ThreadTaskRunner& ThreadTaskRunner::operator=(ThreadTaskRunner&& other) {
  this->~ThreadTaskRunner();
  new (this) ThreadTaskRunner(std::move(other));
  return *this;
}

ThreadTaskRunner::~ThreadTaskRunner() {
  if (task_runner_) {
    PERFETTO_CHECK(!task_runner_->QuitCalled());
    task_runner_->Quit();

    PERFETTO_DCHECK(thread_.joinable());
  }
  if (thread_.joinable())
    thread_.join();
}

ThreadTaskRunner::ThreadTaskRunner() {
  std::mutex init_lock;
  std::condition_variable init_cv;

  std::function<void(UnixTaskRunner*)> initializer =
      [this, &init_lock, &init_cv](UnixTaskRunner* task_runner) {
        std::lock_guard<std::mutex> lock(init_lock);
        task_runner_ = task_runner;
        // Notify while still holding the lock, as init_cv ceases to exist as
        // soon as the main thread observes a non-null task_runner_, and it can
        // wake up spuriously (i.e. before the notify if we had unlocked before
        // notifying).
        init_cv.notify_one();
      };
  thread_ = std::thread(&ThreadTaskRunner::RunTaskThread, this,
                        std::move(initializer));

  std::unique_lock<std::mutex> lock(init_lock);
  init_cv.wait(lock, [this] { return !!task_runner_; });
}

void ThreadTaskRunner::RunTaskThread(
    std::function<void(UnixTaskRunner*)> initializer) {
  UnixTaskRunner task_runner;
  task_runner.PostTask(std::bind(std::move(initializer), &task_runner));
  task_runner.Run();
}

}  // namespace base
}  // namespace perfetto
