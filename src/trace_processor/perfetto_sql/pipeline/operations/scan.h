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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_SCAN_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_SCAN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/sqlite/sql_source.h"

struct SyntaqlitePerfettoPipeSource;

namespace perfetto::trace_processor::core::exec {
class Source;
}  // namespace perfetto::trace_processor::core::exec

namespace perfetto::trace_processor::core::dataframe {
class Dataframe;
}  // namespace perfetto::trace_processor::core::dataframe

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// A relation a source reads: the plan node producing it and its columns.
struct SourceRelation {
  PlanNodeId node = 0;
  std::vector<NamedColumn> columns;
};

// A dataframe read directly. Defined outside Scan because GCC only treats a
// nested class's member initializers as parsed once the enclosing class is.
struct ScanDataframe {
  std::string name;
  // Resolved at compile time; only the selected columns are retained.
  std::vector<std::shared_ptr<const dataframe::Column>> columns;
  uint32_t row_count = 0;
};

// The dataframe the plan is passed as its `index`-th argument when it runs.
// SQLite builds it from a relation the plan read from SQL.
struct ScanDataframeArg {
  uint32_t index = 0;
};

// Reads all rows of a source. Always the first operation.
class Scan : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

  // Builds the plan for what a source reads: a dataframe or SQL.
  static base::StatusOr<SourceRelation> BuildRelation(Compiler*,
                                                      uint32_t source);

  // The name a source's columns can be qualified with, if it has one.
  static std::optional<std::string> SourceQualifier(
      Compiler*,
      const SyntaqlitePerfettoPipeSource& n);
  std::unique_ptr<core::exec::Source> MakeSource() const;

  // Intersection embeds a scan payload without a separate source tag.
  static Available ReadPayload(PlanReader*, Scan*);
  const std::vector<NamedColumn>& columns() const { return columns_; }

 private:
  using Dataframe = ScanDataframe;
  using DataframeArg = ScanDataframeArg;
  // Where a scan reads from: a dataframe, SQL, or a dataframe argument. SQL
  // sources become dataframe arguments when the plan is written into SQL, and
  // those are bound to dataframes when it is loaded.
  using Source = std::variant<Dataframe, SqlSource, DataframeArg>;

  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;
  // Materializes SQL sources for execution tests without exposing the payload.
  friend class TestCatalog;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Write(PlanWriter*, const PlanNode&, Available*) const override;
  void MoveSqlSourcesToDataframeArgs(std::vector<std::string>*) override;
  base::Status ResolveDataframes(LogicalPlan*, const Catalog&) override;
  base::Status BindDataframeArgs(
      LogicalPlan*,
      const std::vector<const dataframe::Dataframe*>&,
      StringPool*) override;

  static base::Status BuildPlan(Compiler*, uint32_t);
  static void DecodePlan(PlanReader*, Available*);

  // The finalized dataframe a source names, if it can be read without SQLite.
  static const dataframe::Dataframe* FindDirectDataframe(
      Compiler*,
      const SyntaqlitePerfettoPipeSource& n);

  // Fails unless every column of a source has a name a pipeline can use.
  // Crossing from SQL into a pipeline needs proper names, as creating a
  // PERFETTO TABLE does.
  static base::Status CheckSourceNames(Compiler*,
                                       const std::vector<NamedColumn>& columns,
                                       uint32_t at);
  static Scan BuildDataframeScan(Compiler*,
                                 const dataframe::Dataframe& dataframe,
                                 std::string name);
  static base::StatusOr<Scan> BuildSqlScan(Compiler*, uint32_t from);
  static void AddScanColumn(Compiler*, Scan* scan, ColumnSchema column);

  const OperationRegistration& registration() const override;

  Source source_;
  // Bindings in source column order.
  std::vector<NamedColumn> columns_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_SCAN_H_
