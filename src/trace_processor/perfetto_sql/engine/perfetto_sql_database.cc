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
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/sqlite/sqlite_database.h"
#include "src/trace_processor/util/sql_modules.h"

namespace perfetto::trace_processor {

PerfettoSqlDatabase::PerfettoSqlDatabase(StringPool* pool)
    : pool_(pool), sqlite_database_(std::make_shared<SqliteDatabase>()) {}

PerfettoSqlDatabase::~PerfettoSqlDatabase() = default;

void PerfettoSqlDatabase::RegisterPackage(
    const std::string& name,
    sql_modules::RegisteredPackage package) {
  packages_.Erase(name);
  packages_.Insert(name, std::move(package));
}

void PerfettoSqlDatabase::ErasePackage(const std::string& name) {
  packages_.Erase(name);
}

sql_modules::RegisteredPackage* PerfettoSqlDatabase::FindPackage(
    const std::string& name) {
  return packages_.Find(name);
}

const sql_modules::RegisteredPackage* PerfettoSqlDatabase::FindPackage(
    const std::string& name) const {
  return packages_.Find(name);
}

sql_modules::RegisteredPackage* PerfettoSqlDatabase::FindPackageForModule(
    const std::string& key) {
  // Find the package whose name is a prefix of the key. Due to prefix clash
  // checking during registration, at most one package can match any given key.
  for (auto pkg = packages_.GetIterator(); pkg; ++pkg) {
    if (sql_modules::IsPackagePrefixOf(pkg.key(), key)) {
      return &pkg.value();
    }
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::string>>
PerfettoSqlDatabase::GetModules() const {
  std::vector<std::pair<std::string, std::string>> result;
  for (auto pkg = packages_.GetIterator(); pkg; ++pkg) {
    for (auto mod = pkg.value().modules.GetIterator(); mod; ++mod) {
      result.emplace_back(pkg.key(), mod.key());
    }
  }
  return result;
}

}  // namespace perfetto::trace_processor
