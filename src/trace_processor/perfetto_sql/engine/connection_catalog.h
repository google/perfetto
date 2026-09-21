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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CONNECTION_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CONNECTION_CATALOG_H_

#include <optional>
#include <string>
#include <string_view>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/pipeline/catalog.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor {

// Adapts a connection to semantic analysis and pipeline compilation. Each
// dataframe is served as a typed leaf relation and as a dataframe.
class ConnectionCatalog final : public perfetto_sql::analysis::Catalog,
                                public pipeline::Catalog {
 public:
  explicit ConnectionCatalog(PerfettoSqlConnection*);

  std::optional<perfetto_sql::analysis::LeafRelation> FindLeafRelation(
      std::string_view name) const override;
  std::optional<std::string> FindViewSql(std::string_view name) const override;

  const dataframe::Dataframe* FindDataframe(
      std::string_view name) const override;
  base::StatusOr<pipeline::Schema> DescribeQuery(
      const SqlSource& sql) const override;

 private:
  PerfettoSqlConnection* connection_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_CONNECTION_CATALOG_H_
