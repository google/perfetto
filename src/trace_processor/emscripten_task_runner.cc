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

#include "src/trace_processor/emscripten_task_runner.h"

#include <emscripten/emscripten.h>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

namespace {
EmscriptenTaskRunner* g_instance = nullptr;

void DoRunNextTask(void*) {
  if (g_instance)
    g_instance->RunNextTask();
}

void DoRunNextDelayedTask(void*) {
  if (g_instance)
    g_instance->RunNextDelayedTask();
}

}  // namespace

EmscriptenTaskRunner::EmscriptenTaskRunner() {
  PERFETTO_CHECK(!g_instance);
  g_instance = this;
}

EmscriptenTaskRunner::~EmscriptenTaskRunner() {
  PERFETTO_CHECK(g_instance == this);
  g_instance = nullptr;
}

void EmscriptenTaskRunner::PostTask(std::function<void()> task) {
  immediate_tasks_.emplace_back(std::move(task));
  emscripten_async_call(&DoRunNextTask, nullptr, 0);
}

void EmscriptenTaskRunner::PostDelayedTask(std::function<void()> task,
                                           uint32_t delay_ms) {
  delayed_tasks_.emplace_back(std::move(task));
  emscripten_async_call(&DoRunNextDelayedTask, nullptr,
                        static_cast<int>(delay_ms));
}

void EmscriptenTaskRunner::RunNextDelayedTask() {
  if (delayed_tasks_.empty()) {
    PERFETTO_DCHECK(false);
    return;
  }
  auto task = std::move(delayed_tasks_.front());
  delayed_tasks_.pop_front();
  task();
}

void EmscriptenTaskRunner::RunNextTask() {
  if (immediate_tasks_.empty()) {
    PERFETTO_DCHECK(false);
    return;
  }
  auto task = std::move(immediate_tasks_.front());
  immediate_tasks_.pop_front();
  task();
}

void EmscriptenTaskRunner::AddFileDescriptorWatch(int, std::function<void()>) {
  PERFETTO_FATAL("FD watches are not supported");
}

void EmscriptenTaskRunner::RemoveFileDescriptorWatch(int) {
  PERFETTO_FATAL("FD watches are not supported");
}

}  // namespace trace_processor
}  // namespace perfetto
