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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_CONNECTION_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_CONNECTION_CATALOG_H_

#include <optional>
#include <string>
#include <string_view>

#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"

namespace perfetto::trace_processor::lineage {

namespace analysis = ::perfetto::perfetto_sql::analysis;

// Adapts the dataframes and SQLite views of a live connection to the reusable
// PerfettoSQL relation analyzer.
class ConnectionCatalog final : public analysis::Catalog {
 public:
  explicit ConnectionCatalog(PerfettoSqlConnection*);

  std::optional<analysis::LeafRelation> FindLeafRelation(
      std::string_view name) const override;
  std::optional<std::string> FindViewSql(std::string_view name) const override;

  // Maps all origins of a result column back to dataframe storage. Returns
  // nothing unless every origin has the same storage type.
  std::optional<core::StorageType> ColumnType(
      const analysis::ColumnLineage&) const;

 private:
  PerfettoSqlConnection* connection_;
};

}  // namespace perfetto::trace_processor::lineage

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_CONNECTION_CATALOG_H_
