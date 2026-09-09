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

// Wraps the task runner that drains the in-process v2 bridges, adding one
// thing: a way to stop accepting work before the runner is destroyed.
//
// Tasks come from arbitrary SDK threads (ring buffer notifications, writer
// destruction), from the muxer sequence (flush and stop barriers) and from the
// relay itself. Some of them race with Tracing::Shutdown(), e.g. the TLS
// v2 writers destroyed during shutdown request destruction of their v1 writers.
//
// Holding |mutex_| across the post means that a caller either gets its task
// onto the live runner, or is told that the sequence is closed and has to deal
// with it. It can never post to a runner that Close() has already handed back
// to the caller.
//
// Bridges hold a shared_ptr to this, so a bridge that outlives shutdown sees a
// closed sequence rather than a dangling pointer.
class RelaySequence {
 public:
  explicit RelaySequence(std::unique_ptr<base::TaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {
    PERFETTO_CHECK(task_runner_);
  }

  RelaySequence(const RelaySequence&) = delete;
  RelaySequence& operator=(const RelaySequence&) = delete;

  // Queues |task| on the relay. Returns false if the sequence is closed, in
  // which case |task| is dropped and the caller is responsible for completing
  // whatever was waiting on it. Thread-safe.
  bool PostTask(std::function<void()> task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!task_runner_)
      return false;
    task_runner_->PostTask(std::move(task));
    return true;
  }

  // Stops accepting tasks and returns the runner to its owner. Posts after
  // this return false. Tasks already queued are neither run nor cancelled
  // here; what happens to them is up to the runner's destructor.
  //
  // The caller must destroy the runner outside both the relay and muxer
  // sequences, and keep the muxer and its endpoints alive until destruction
  // returns, as queued tasks may still run.
  //
  // Destroying the runner cannot deadlock on the muxer: the v1 writers the
  // bridges forward into use kDrop, so a drain never waits for the SMB to be
  // freed.
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
