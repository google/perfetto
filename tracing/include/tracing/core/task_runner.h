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

#ifndef TRACING_INCLUDE_TRACING_CORE_TASK_RUNNER_PROXY_H_
#define TRACING_INCLUDE_TRACING_CORE_TASK_RUNNER_PROXY_H_

#include <functional>

namespace perfetto {

// A generic interface to allow the library clients to interleave the execution
// of the tracing internals in their runtime environment.
// The expectation is that all tasks, which are queued either via PostTask() or
// AddFileDescriptorWatch(), are executed on the same sequence (either on the
// same thread, or on a thread pool that gives sequencing guarantees).
//
// Exposed to:
//  Most internal classes under src/.
//
// Subclassed by:
//   1. Library clients.
//   2. Tests (See test/test_task_runner.h)

// TODO(skyostil): rework this.
// TODO: we should provide a reference implementation that just spins a
// dedicated thread. For the moment the only implementation is in
// test/test_task_runner.h.
class TaskRunner {
 public:
  virtual ~TaskRunner() = default;

  virtual void PostTask(std::function<void()>) = 0;
  virtual void AddFileDescriptorWatch(int fd, std::function<void()>) = 0;
  virtual void RemoveFileDescriptorWatch(int fd) = 0;
};

}  // namespace perfetto

#endif  // TRACING_INCLUDE_TRACING_CORE_TASK_RUNNER_PROXY_H_
