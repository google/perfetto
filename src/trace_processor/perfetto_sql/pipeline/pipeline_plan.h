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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_PLAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_PLAN_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/perfetto_sql/pipeline/pipeline_syntax.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::pipeline {

// What planning needs from the connection a pipeline will run on. The
// connection and the pool must outlive the plan; the catalog is only used
// while planning.
struct PlanEnvironment {
  SqliteConnection* connection = nullptr;
  StringPool* pool = nullptr;
  // Traces SQL result columns back to dataframes, which gives them a type.
  const perfetto_sql::analysis::Catalog* catalog = nullptr;
  // Returns the dataframe registered under a table name, or null.
  std::function<std::shared_ptr<const dataframe::Dataframe>(std::string_view)>
      find_dataframe;
};

// A pipeline ready to run: the plan nodes it is built from, and which of the
// columns its batches carry are its output.
//
// The plan is const once built, so it can be run any number of times, one
// state per run.
class PipelinePlan {
 public:
  struct Column {
    std::string name;
    // Where in a batch of source() the column is.
    uint32_t index;
  };

  PipelinePlan();
  ~PipelinePlan();
  PipelinePlan(const PipelinePlan&) = delete;
  PipelinePlan& operator=(const PipelinePlan&) = delete;

  const core::exec::Source& source() const { return *nodes_.back(); }
  const std::vector<Column>& columns() const { return columns_; }

 private:
  friend class Planner;

  // Each node reads from nodes before it, so the last is the root.
  std::vector<std::unique_ptr<core::exec::Source>> nodes_;
  std::vector<Column> columns_;
  // The dataframes scanned directly. SQLite does not know the plan reads
  // them, so nothing stops their table being replaced while it runs.
  std::vector<std::shared_ptr<const dataframe::Dataframe>> dataframes_;
};

// Plans `syntax`.
//
// The FROM clause is read straight from the dataframe when it names one, and
// through SQLite otherwise. Every error names what was wrong and where.
base::StatusOr<std::unique_ptr<PipelinePlan>> PlanPipeline(
    const PipelineSyntax& syntax,
    const PlanEnvironment& env);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_PLAN_H_
