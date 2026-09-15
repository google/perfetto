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

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::lineage {

namespace analysis = ::perfetto::perfetto_sql::analysis;

// Adapts one trace processor connection to semantic analysis. Serves each
// dataframe as a leaf relation whose columns carry their storage type.
//
// Takes the SQLite connection and a way to find dataframes rather than the
// PerfettoSQL connection itself, so the PerfettoSQL engine can depend on it.
class ConnectionCatalog final : public analysis::Catalog {
 public:
  using DataframeLookup =
      std::function<const dataframe::Dataframe*(std::string_view)>;

  ConnectionCatalog(SqliteConnection*, DataframeLookup);

  std::optional<analysis::LeafRelation> FindLeafRelation(
      std::string_view name) const override;
  std::optional<std::string> FindViewSql(std::string_view name) const override;

 private:
  SqliteConnection* connection_;
  DataframeLookup find_dataframe_;
};

}  // namespace perfetto::trace_processor::lineage

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_CONNECTION_CATALOG_H_
