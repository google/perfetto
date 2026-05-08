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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/preprocessor/perfetto_sql_preprocessor.h"
#include "src/trace_processor/sqlite/committed_state_manager.h"
#include "src/trace_processor/sqlite/sqlite_database.h"
#include "src/trace_processor/util/sql_modules.h"

namespace perfetto::trace_processor {

// Database-scoped state shared by every |PerfettoSqlConnection| attached to
// it: the underlying |SqliteDatabase|, package/macro registries, and the
// committed view of per-vtab state for each vtab module.
class PerfettoSqlDatabase {
 public:
  using Macro = PerfettoSqlPreprocessor::Macro;

  explicit PerfettoSqlDatabase(StringPool* pool);
  ~PerfettoSqlDatabase();

  PerfettoSqlDatabase(const PerfettoSqlDatabase&) = delete;
  PerfettoSqlDatabase& operator=(const PerfettoSqlDatabase&) = delete;

  StringPool* pool() const { return pool_; }
  std::shared_ptr<SqliteDatabase> sqlite_database() const {
    return sqlite_database_;
  }

  void RegisterPackage(const std::string& name,
                       sql_modules::RegisteredPackage package);
  void ErasePackage(const std::string& name);
  sql_modules::RegisteredPackage* FindPackage(const std::string& name);
  const sql_modules::RegisteredPackage* FindPackage(
      const std::string& name) const;
  sql_modules::RegisteredPackage* FindPackageForModule(const std::string& key);
  std::vector<std::pair<std::string, std::string>> GetModules() const;
  base::FlatHashMap<std::string, sql_modules::RegisteredPackage>& packages() {
    return packages_;
  }

  base::FlatHashMap<std::string, Macro>& macros() { return macros_; }
  const base::FlatHashMap<std::string, Macro>& macros() const {
    return macros_;
  }
  size_t macro_count() const { return macros_.size(); }

  // One committed-state store per vtab module type. Per-connection
  // |sqlite::ModuleStateManager|s are constructed against these so the
  // committed view is shared across every connection on this database.
  sqlite::CommittedStateManager& committed_dataframes() {
    return committed_dataframes_;
  }
  sqlite::CommittedStateManager& committed_runtime_table_functions() {
    return committed_runtime_table_functions_;
  }
  sqlite::CommittedStateManager& committed_static_table_functions() {
    return committed_static_table_functions_;
  }

 private:
  StringPool* const pool_;
  std::shared_ptr<SqliteDatabase> sqlite_database_;
  base::FlatHashMap<std::string, sql_modules::RegisteredPackage> packages_;
  base::FlatHashMap<std::string, Macro> macros_;

  sqlite::CommittedStateManager committed_dataframes_;
  sqlite::CommittedStateManager committed_runtime_table_functions_;
  sqlite::CommittedStateManager committed_static_table_functions_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_PERFETTO_SQL_DATABASE_H_
