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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/lineage/column_lineage.h"

namespace perfetto::trace_processor::lineage {

// Resolves FROM clause names against a live connection: its dataframes and
// the views SQLite holds for it.
class ConnectionCatalog : public Catalog {
 public:
  explicit ConnectionCatalog(PerfettoSqlConnection*);
  ~ConnectionCatalog() override;

  const std::vector<ResolvedColumn>* Dataframe(
      const std::string& name) const override;
  std::optional<std::string> ViewSql(const std::string& name) const override;

 private:
  PerfettoSqlConnection* connection_;
  // Cached because the columns are handed back by pointer, and because the
  // same name is usually looked up more than once.
  mutable std::map<std::string, std::vector<ResolvedColumn>> dataframes_;
};

}  // namespace perfetto::trace_processor::lineage

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_LINEAGE_CONNECTION_CATALOG_H_
