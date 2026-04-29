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

#include "src/trace_processor/perfetto_sql/engine/global_staging_area.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace perfetto::trace_processor {

GlobalStagingArea::GlobalStagingArea() = default;
GlobalStagingArea::~GlobalStagingArea() = default;

GlobalStagingArea::IncludeLockGuard GlobalStagingArea::AcquireIncludeLock(
    const std::string& module_name) {
  std::mutex* module_mutex;
  {
    std::lock_guard<std::mutex> guard(map_mutex_);
    auto it = module_locks_.find(module_name);
    if (it == module_locks_.end()) {
      auto inserted = module_locks_.emplace(
          module_name, std::make_unique<std::mutex>());
      it = inserted.first;
    }
    module_mutex = it->second.get();
  }
  return IncludeLockGuard(std::unique_lock<std::mutex>(*module_mutex));
}

}  // namespace perfetto::trace_processor
