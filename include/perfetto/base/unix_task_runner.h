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

#ifndef INCLUDE_PERFETTO_BASE_UNIX_TASK_RUNNER_H_
#define INCLUDE_PERFETTO_BASE_UNIX_TASK_RUNNER_H_

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_checker.h"
#include "perfetto/base/time.h"

#include <poll.h>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace perfetto {
namespace base {

// Runs a task runner on the current thread.
class UnixTaskRunner : public TaskRunner {
 public:
  UnixTaskRunner();
  ~UnixTaskRunner() override;

  // Start executing tasks. Doesn't return until Quit() is called. Run() may be
  // called multiple times on the same task runner.
  void Run();
  void Quit();

  // Checks whether there are any pending immediate tasks to run. Note that
  // delayed tasks don't count even if they are due to run.
  bool IsIdleForTesting();

  // TaskRunner implementation:
  void PostTask(std::function<void()>) override;
  void PostDelayedTask(std::function<void()>, uint32_t delay_ms) override;
  void AddFileDescriptorWatch(int fd, std::function<void()>) override;
  void RemoveFileDescriptorWatch(int fd) override;

 private:
  void WakeUp();

  void UpdateWatchTasksLocked();

  int GetDelayMsToNextTaskLocked() const;
  void RunImmediateAndDelayedTask();
  void PostFileDescriptorWatches();
  void RunFileDescriptorWatch(int fd);

  ThreadChecker thread_checker_;

  ScopedFile control_read_;
  ScopedFile control_write_;

  std::vector<struct pollfd> poll_fds_;

  // --- Begin lock-protected members ---

  std::mutex lock_;

  std::deque<std::function<void()>> immediate_tasks_;
  std::multimap<TimeMillis, std::function<void()>> delayed_tasks_;
  bool quit_ = false;

  struct WatchTask {
    std::function<void()> callback;
    size_t poll_fd_index;  // Index into |poll_fds_|.
  };

  std::map<int, WatchTask> watch_tasks_;
  bool watch_tasks_changed_ = false;

  // --- End lock-protected members ---
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_UNIX_TASK_RUNNER_H_
