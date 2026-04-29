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
  std::recursive_mutex* module_mutex;
  {
    std::lock_guard<std::mutex> guard(map_mutex_);
    auto it = module_locks_.find(module_name);
    if (it == module_locks_.end()) {
      auto inserted = module_locks_.emplace(
          module_name, std::make_unique<std::recursive_mutex>());
      it = inserted.first;
    }
    module_mutex = it->second.get();
  }
  return IncludeLockGuard(
      std::unique_lock<std::recursive_mutex>(*module_mutex));
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

uint64_t GlobalStagingArea::AppendFunction(FunctionPoolEntry entry) {
  std::lock_guard<std::mutex> guard(function_pool_mutex_);
  function_pool_.push_back(std::move(entry));
  uint64_t new_version = function_pool_.size();
  // Publish the new version *after* the entry is in the vector so a reader
  // that observes `LatestFunctionVersion() >= new_version` is guaranteed to
  // see the entry at index `new_version - 1` once it acquires the mutex.
  function_pool_version_.store(new_version, std::memory_order_release);
  return new_version;
}

GlobalStagingArea::FunctionPoolSnapshot GlobalStagingArea::SnapshotSince(
    uint64_t since_version) const {
  // Fast-path: peek the atomic; if the caller is already up-to-date we
  // don't even need to take the lock. This makes `Execute` start cheap on
  // connections that have already caught up.
  if (function_pool_version_.load(std::memory_order_acquire) <= since_version) {
    return {{}, since_version};
  }

  std::lock_guard<std::mutex> guard(function_pool_mutex_);
  FunctionPoolSnapshot snapshot;
  snapshot.latest_version = function_pool_.size();
  if (since_version >= snapshot.latest_version) {
    return snapshot;
  }
  snapshot.entries.reserve(snapshot.latest_version - since_version);
  for (size_t i = since_version; i < function_pool_.size(); ++i) {
    // FunctionPoolEntry is copy-constructible (FunctionPrototype + SqlSource
    // are value types). We deliberately copy here rather than move-out: the
    // pool is additive and must keep its entries for future readers that
    // catch up later.
    snapshot.entries.push_back(function_pool_[i]);
  }
  return snapshot;
}

uint64_t GlobalStagingArea::LatestFunctionVersion() const {
  return function_pool_version_.load(std::memory_order_acquire);
}

void GlobalStagingArea::ResetFunctionPool() {
  std::lock_guard<std::mutex> guard(function_pool_mutex_);
  function_pool_.clear();
  function_pool_version_.store(0, std::memory_order_release);
}

uint64_t GlobalStagingArea::AppendPackage(PackagePoolEntry entry) {
  std::lock_guard<std::mutex> guard(package_pool_mutex_);
  package_pool_.push_back(std::move(entry));
  uint64_t new_version = package_pool_.size();
  // Publish after the entry is in the vector — same release-after-write
  // ordering as the function pool.
  package_pool_version_.store(new_version, std::memory_order_release);
  return new_version;
}

GlobalStagingArea::PackagePoolSnapshot GlobalStagingArea::SnapshotPackagesSince(
    uint64_t since_version) const {
  // Fast-path peek of the atomic; no lock needed if the caller is already
  // up-to-date.
  if (package_pool_version_.load(std::memory_order_acquire) <= since_version) {
    return {{}, since_version};
  }

  std::lock_guard<std::mutex> guard(package_pool_mutex_);
  PackagePoolSnapshot snapshot;
  snapshot.latest_version = package_pool_.size();
  if (since_version >= snapshot.latest_version) {
    return snapshot;
  }
  snapshot.entries.reserve(snapshot.latest_version - since_version);
  for (size_t i = since_version; i < package_pool_.size(); ++i) {
    // PackagePoolEntry is cheaply copyable: `package` is a `shared_ptr` so
    // the underlying RegisteredPackage payload is reference-counted and not
    // duplicated.
    snapshot.entries.push_back(package_pool_[i]);
  }
  return snapshot;
}

uint64_t GlobalStagingArea::LatestPackageVersion() const {
  return package_pool_version_.load(std::memory_order_acquire);
}

void GlobalStagingArea::ResetPackagePool() {
  std::lock_guard<std::mutex> guard(package_pool_mutex_);
  package_pool_.clear();
  package_pool_version_.store(0, std::memory_order_release);
}

void GlobalStagingArea::MarkModuleIncluded(const std::string& key) {
  std::lock_guard<std::mutex> guard(included_modules_mutex_);
  included_modules_.Erase(key);
  included_modules_.Insert(key, true);
}

bool GlobalStagingArea::IsModuleIncluded(const std::string& key) const {
  std::lock_guard<std::mutex> guard(included_modules_mutex_);
  return included_modules_.Find(key) != nullptr;
}

void GlobalStagingArea::ResetIncludedModules() {
  std::lock_guard<std::mutex> guard(included_modules_mutex_);
  included_modules_.Clear();
}

}  // namespace perfetto::trace_processor
