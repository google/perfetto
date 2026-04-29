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

std::string GlobalStagingArea::MakeVtabKey(const std::string& module_name,
                                           const std::string& vtab_name) {
  std::string key;
  key.reserve(module_name.size() + 1 + vtab_name.size());
  key.append(module_name);
  key.push_back('\0');
  key.append(vtab_name);
  return key;
}

void GlobalStagingArea::PublishVtabState(const std::string& module_name,
                                         const std::string& vtab_name,
                                         std::shared_ptr<void> state) {
  std::lock_guard<std::mutex> guard(vtab_state_mutex_);
  std::string key = MakeVtabKey(module_name, vtab_name);
  vtab_state_.Erase(key);
  vtab_state_.Insert(std::move(key), std::move(state));
}

void GlobalStagingArea::RemoveVtabState(const std::string& module_name,
                                        const std::string& vtab_name) {
  std::lock_guard<std::mutex> guard(vtab_state_mutex_);
  vtab_state_.Erase(MakeVtabKey(module_name, vtab_name));
}

std::shared_ptr<void> GlobalStagingArea::LookupVtabState(
    const std::string& module_name,
    const std::string& vtab_name) const {
  std::lock_guard<std::mutex> guard(vtab_state_mutex_);
  auto* found = vtab_state_.Find(MakeVtabKey(module_name, vtab_name));
  if (!found) {
    return nullptr;
  }
  return *found;
}

}  // namespace perfetto::trace_processor
