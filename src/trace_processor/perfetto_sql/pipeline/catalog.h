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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_

#include <string_view>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/sqlite/sql_source.h"

namespace perfetto::trace_processor::pipeline {

// Lookup interface the compiler uses to resolve what a pipeline reads.
class Catalog {
 public:
  virtual ~Catalog();

  // Dataframe registered as `name`, or null.
  virtual const dataframe::Dataframe* FindDataframe(
      std::string_view name) const = 0;

  // Columns of the query `sql`, typed where they trace back to a dataframe
  // column. Fails if SQLite cannot prepare the query.
  virtual base::StatusOr<Schema> DescribeQuery(const SqlSource& sql) const = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
