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

#ifndef SRC_TRACE_PROCESSOR_EMSCRIPTEN_TASK_RUNNER_H_
#define SRC_TRACE_PROCESSOR_EMSCRIPTEN_TASK_RUNNER_H_

#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"

#include <list>

namespace perfetto {
namespace trace_processor {

class EmscriptenTaskRunner : public base::TaskRunner {
 public:
  EmscriptenTaskRunner();
  ~EmscriptenTaskRunner() override;

  void PostTask(std::function<void()>) override;
  void PostDelayedTask(std::function<void()>, uint32_t delay_ms) override;

  void AddFileDescriptorWatch(int fd, std::function<void()>) override;
  void RemoveFileDescriptorWatch(int fd) override;

  void RunNextTask();
  void RunNextDelayedTask();

 private:
  std::list<std::function<void()>> immediate_tasks_;
  std::list<std::function<void()>> delayed_tasks_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_EMSCRIPTEN_TASK_RUNNER_H_
