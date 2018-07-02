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

#ifndef INCLUDE_PERFETTO_BASE_TASK_RUNNER_H_
#define INCLUDE_PERFETTO_BASE_TASK_RUNNER_H_

#include <functional>

#include "perfetto/base/build_config.h"
#include "perfetto/base/export.h"
#include "perfetto/base/utils.h"
#include "perfetto/base/watchdog.h"

namespace perfetto {
namespace base {

// Maximum time a single task can take in a TaskRunner before the
// program suicides.
constexpr int64_t kWatchdogMillis = 30000;  // 30s

// A generic interface to allow the library clients to interleave the execution
// of the tracing internals in their runtime environment.
// The expectation is that all tasks, which are queued either via PostTask() or
// AddFileDescriptorWatch(), are executed on the same sequence (either on the
// same thread, or on a thread pool that gives sequencing guarantees).
//
// Tasks are never executed synchronously inside PostTask and there is a full
// memory barrier between tasks.
//
// All methods of this interface can be called from any thread.
class PERFETTO_EXPORT TaskRunner {
 public:
  virtual ~TaskRunner();

  // Schedule a task for immediate execution. Immediate tasks are always
  // executed in the order they are posted. Can be called from any thread.
  virtual void PostTask(std::function<void()>) = 0;

  // Schedule a task for execution after |delay_ms|. Note that there is no
  // strict ordering guarantee between immediate and delayed tasks. Can be
  // called from any thread.
  virtual void PostDelayedTask(std::function<void()>, uint32_t delay_ms) = 0;

  // Schedule a task to run when |fd| becomes readable. The same |fd| can only
  // be monitored by one function. Note that this function only needs to be
  // implemented on platforms where the built-in ipc framework is used. Can be
  // called from any thread.
  // TODO(skyostil): Refactor this out of the shared interface.
  virtual void AddFileDescriptorWatch(int fd, std::function<void()>) = 0;

  // Remove a previously scheduled watch for |fd|. If this is run on the target
  // thread of this TaskRunner, guarantees that the task registered to this fd
  // will not be executed after this function call. Can be called from any
  // thread.
  virtual void RemoveFileDescriptorWatch(int fd) = 0;

 protected:
  static void RunTask(const std::function<void()>& task) {
    Watchdog::Timer handle =
        base::Watchdog::GetInstance()->CreateFatalTimer(kWatchdogMillis);
    task();
  }
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_TASK_RUNNER_H_
