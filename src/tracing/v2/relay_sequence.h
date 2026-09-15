/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACING_V2_RELAY_SEQUENCE_H_
#define SRC_TRACING_V2_RELAY_SEQUENCE_H_

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"

namespace perfetto::tracing_v2 {

// Lets the v2 relay stop accepting tasks before its runner is destroyed.
//
// SDK threads, the muxer and the relay itself post tasks here. Posts can race
// with Tracing::Shutdown(), including requests to destroy retained v1 writers.
//
// PostTask() and Close() hold the same mutex:
// - If PostTask() gets the lock first, it posts before Close() removes the
//   runner.
// - If Close() gets the lock first, PostTask() sees no runner and rejects the
//   task.
//
// Bridges retain this handle after shutdown. Later posts fail safely.
class RelaySequence {
 public:
  explicit RelaySequence(std::unique_ptr<base::TaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {
    PERFETTO_CHECK(task_runner_);
  }

  RelaySequence(const RelaySequence&) = delete;
  RelaySequence& operator=(const RelaySequence&) = delete;

  // Queues |task| on the relay. Returns false and drops it after Close().
  // The caller decides how to handle rejected work. Thread-safe.
  bool PostTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!task_runner_)
      return false;
    task_runner_->PostTask(std::move(task));
    return true;
  }

  // Stops accepting tasks and returns ownership of the runner to the caller.
  // Later PostTask() calls return false. Close() leaves queued tasks untouched.
  // The runner's destructor determines whether to run or discard those tasks.
  //
  // The caller must destroy the runner outside both the relay and muxer
  // sequences. Keep the muxer and its endpoints alive until destruction returns
  // because queued tasks can still use them.
  //
  // The bridge's v1 writers use kDrop. A drain therefore does not wait for the
  // muxer to free SMB space while the caller waits for runner destruction.
  std::unique_ptr<base::TaskRunner> Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_runner_)
      PERFETTO_CHECK(!task_runner_->RunsTasksOnCurrentThread());
    return std::move(task_runner_);
  }

 private:
  std::mutex mutex_;
  std::unique_ptr<base::TaskRunner> task_runner_;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_RELAY_SEQUENCE_H_
