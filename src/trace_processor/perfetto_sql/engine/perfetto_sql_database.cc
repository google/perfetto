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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_database.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "src/trace_processor/util/sql_modules.h"

namespace perfetto::trace_processor {

sql_modules::RegisteredPackage* PerfettoSqlDatabase::FindPackageForModule(
    const std::string& module_key) {
  // Prefix-match: `RegisterSqlPackage` rejects clashing prefixes so at
  // most one package can match.
  for (auto pkg = packages_.GetIterator(); pkg; ++pkg) {
    if (sql_modules::IsPackagePrefixOf(pkg.key(), module_key)) {
      return &pkg.value();
    }
  }
  return nullptr;
}

PerfettoSqlDatabase::PerfettoSqlDatabase() = default;
PerfettoSqlDatabase::~PerfettoSqlDatabase() = default;

PerfettoSqlDatabase::IncludeClaimResult PerfettoSqlDatabase::TryClaimInclude(
    const std::string& key) {
  std::unique_lock<std::mutex> lock(include_mu_);
  include_cv_.wait(lock, [&] { return in_progress_.count(key) == 0; });
  if (included_modules_.count(key)) {
    return {{}, /*already_included=*/true};
  }
  in_progress_.insert(key);
  return {IncludeClaim(this, key), /*already_included=*/false};
}

void PerfettoSqlDatabase::ReleaseClaim(const std::string& key, bool success) {
  {
    std::lock_guard<std::mutex> lock(include_mu_);
    in_progress_.erase(key);
    if (success) {
      included_modules_.insert(key);
    }
  }
  include_cv_.notify_all();
}

void PerfettoSqlDatabase::IncludeClaim::Reset(bool success) {
  if (db_) {
    db_->ReleaseClaim(key_, success);
    db_ = nullptr;
    key_.clear();
  }
}

std::string PerfettoSqlDatabase::MakeVtabKey(const std::string& module_name,
                                             const std::string& vtab_name) {
  std::string key;
  key.reserve(module_name.size() + 1 + vtab_name.size());
  key.append(module_name);
  key.push_back('\0');
  key.append(vtab_name);
  return key;
}

void PerfettoSqlDatabase::PublishVtabState(const std::string& module_name,
                                           const std::string& vtab_name,
                                           std::shared_ptr<void> state) {
  std::lock_guard<std::mutex> g(vtab_state_mutex_);
  std::string key = MakeVtabKey(module_name, vtab_name);
  vtab_state_.Erase(key);
  vtab_state_.Insert(std::move(key), std::move(state));
}

void PerfettoSqlDatabase::RemoveVtabState(const std::string& module_name,
                                          const std::string& vtab_name) {
  std::lock_guard<std::mutex> g(vtab_state_mutex_);
  vtab_state_.Erase(MakeVtabKey(module_name, vtab_name));
}

std::shared_ptr<void> PerfettoSqlDatabase::LookupVtabState(
    const std::string& module_name,
    const std::string& vtab_name) const {
  std::lock_guard<std::mutex> g(vtab_state_mutex_);
  auto* found = vtab_state_.Find(MakeVtabKey(module_name, vtab_name));
  return found ? *found : nullptr;
}

void PerfettoSqlDatabase::ResetIncludedModules() {
  std::lock_guard<std::mutex> g(include_mu_);
  included_modules_.clear();
}

}  // namespace perfetto::trace_processor
