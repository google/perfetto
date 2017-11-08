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

#ifndef TRACING_SRC_TEST_TEST_TASK_RUNNER_H_
#define TRACING_SRC_TEST_TEST_TASK_RUNNER_H_

#include <sys/select.h>

#include <functional>
#include <list>
#include <map>
#include <string>

#include "base/task_runner.h"

namespace perfetto {
namespace base {

class TestTaskRunner : public TaskRunner {
 public:
  TestTaskRunner();
  ~TestTaskRunner() override;

  void RunUntilIdle();
  void __attribute__((__noreturn__)) Run();

  std::function<void()> CreateCheckpoint(const std::string& checkpoint);
  void RunUntilCheckpoint(const std::string& checkpoint, int timeout_ms = 5000);

  // TaskRunner implementation.
  void PostTask(std::function<void()> closure) override;
  void AddFileDescriptorWatch(int fd, std::function<void()> callback) override;
  void RemoveFileDescriptorWatch(int fd) override;

 private:
  TestTaskRunner(const TestTaskRunner&) = delete;
  TestTaskRunner& operator=(const TestTaskRunner&) = delete;

  bool RunOneTask();
  void QueueFileDescriptorWatches(bool blocking);

  std::list<std::function<void()>> task_queue_;
  std::map<int, std::function<void()>> watched_fds_;
  std::map<int, bool> fd_watch_task_queued_;
  std::map<std::string, bool> checkpoints_;
};

}  // namespace base
}  // namespace perfetto

#endif  // TRACING_SRC_TEST_TEST_TASK_RUNNER_H_
