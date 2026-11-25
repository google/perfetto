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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_BUILDER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_BUILDER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/aggregate_function.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/prepared_statement.h"

namespace perfetto::trace_processor::core::dataframe {
class Dataframe;
}  // namespace perfetto::trace_processor::core::dataframe

namespace perfetto::trace_processor::pipeline {

class LogicalPlan;

// Constructs a PreparedStatement one relation at a time.
//
// Sources (`Scan`, `ScanTable`, `ScanSql`) produce a `Relation` and named
// `ColumnId` handles; each stage consumes one or more `Relation`s and returns
// the next; `Finish()` prunes unused columns and consumes the builder into a
// PreparedStatement.
class Builder {
 public:
  struct Relation {
    uint32_t id = 0;

    bool operator==(const Relation& other) const { return id == other.id; }
    bool operator!=(const Relation& other) const { return id != other.id; }
  };

  struct ScanResult {
    Relation relation;
    std::vector<NamedColumn> columns;
  };

  struct RelationWithColumns {
    Relation relation;
    std::vector<ColumnId> columns;
  };

  struct TableColumn {
    NameId name = 0;
    uint32_t index = 0;
  };

  struct SqlColumn {
    NameId name = 0;
    PlanType type;
  };

  struct WhereCondition {
    ColumnId column = 0;
    core::Op op{core::Eq{}};
    // Empty for IsNull/IsNotNull; 1 value for scalar ops; 1+ values for IN.
    std::vector<Literal> values;
  };

  struct ComputeItem {
    NameId name = 0;
    PlanType type;
    Expr expr;
  };

  struct AggregateItem {
    using Function = core::exec::AggregateCall::Function;
    NameId name = 0;
    Function function = Function::kCountStar;
    // Ignored for kCountStar.
    ColumnId column = 0;
  };

  struct IntervalOperand {
    Relation relation;
    ColumnId ts = 0;
    ColumnId dur = 0;
    std::vector<ColumnId> keys;
    std::vector<ColumnId> carried;
  };

  struct FillGapsOutput {
    NameId name = 0;
    PlanType type;
    ColumnId input = 0;
    std::optional<ColumnId> background;
  };

  struct TreeKeyPathsResult {
    Relation relation;
    ColumnId path = 0;
    ColumnId parent_path = 0;
  };

  struct GraphTreeResult {
    Relation relation;
    ColumnId parent = 0;
  };

  struct IntervalIntersectResult {
    Relation relation;
    ColumnId ts = 0;
    ColumnId dur = 0;
  };

  struct IntervalFlattenResult {
    Relation relation;
    ColumnId ts = 0;
    ColumnId dur = 0;
    std::vector<ColumnId> aggregates;
  };

  Builder();
  Builder(Builder&&) noexcept;
  Builder& operator=(Builder&&) noexcept;
  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
  ~Builder();

  NameId InternName(std::string_view text);
  const std::string& NameText(NameId name) const;
  bool SameName(NameId a, NameId b) const;

  PlanType ColumnType(ColumnId column) const;
  base::StatusOr<PlanType> CheckExprType(const Expr& expr) const;

  // Allocates a string literal in the plan's arena.
  Literal StringLiteral(std::string_view text);

  // Copies an expression's steps and literals into the plan's arena.
  Expr MakeExpr(base::Span<const core::exec::ExprStep> nodes,
                base::Span<const Literal> literals);

  // Scans `column_indices` of the next positional input in
  // `ExecutionContext::args`. `schema` is inspected only during this call.
  ScanResult Scan(const core::dataframe::Dataframe& schema,
                  base::Span<const uint32_t> column_indices);
  // Scans all non-hidden columns of `schema` as the next positional input.
  ScanResult Scan(const core::dataframe::Dataframe& schema);

  // Scans `columns` of a table resolved from `ExecutionContext::catalog`.
  ScanResult ScanTable(const core::dataframe::Dataframe& schema,
                       std::string_view table_name,
                       base::Span<const TableColumn> columns);

  // Scans a SQL relation hoisted into an argument by `ToSqliteSql()`.
  ScanResult ScanSql(std::string_view sql, base::Span<const SqlColumn> columns);

  Relation Where(Relation input, std::vector<WhereCondition> conditions);
  RelationWithColumns Compute(Relation input, std::vector<ComputeItem> items);
  Relation Distinct(Relation input, base::Span<const ColumnId> keys);
  RelationWithColumns Aggregate(Relation input,
                                base::Span<const ColumnId> keys,
                                base::Span<const AggregateItem> aggregates);
  Relation Join(Relation left,
                Relation right,
                core::exec::JoinSpec::Kind kind,
                ColumnId probe_key,
                ColumnId build_key,
                base::Span<const ColumnId> outputs);
  Relation OrderBy(Relation input, std::vector<SortKey> keys);
  // Negative `count` keeps all rows; negative `offset` skips none.
  Relation Limit(Relation input, int64_t count, int64_t offset = 0);

  RelationWithColumns TreeAccumulate(
      Relation input,
      TreeDirection direction,
      ColumnId node,
      ColumnId parent,
      base::Span<const AggregateItem> aggregates);
  TreeKeyPathsResult TreeKeyPaths(Relation input,
                                  ColumnId node,
                                  ColumnId parent,
                                  base::Span<const ColumnId> keys,
                                  NameId path_name,
                                  NameId parent_path_name);
  GraphTreeResult GraphTree(core::exec::GraphTreeSpec::Kind kind,
                            Relation nodes,
                            ColumnId node,
                            base::Span<const ColumnId> carried,
                            Relation edges,
                            ColumnId source,
                            ColumnId target,
                            Relation seeds,
                            ColumnId seed,
                            std::string_view parent_name = "parent_id");
  IntervalIntersectResult IntervalIntersect(
      const std::vector<IntervalOperand>& operands,
      std::string_view ts_name = "ts",
      std::string_view dur_name = "dur");
  IntervalFlattenResult IntervalFlatten(
      Relation input,
      ColumnId ts,
      ColumnId dur,
      base::Span<const ColumnId> keys,
      base::Span<const AggregateItem> aggregates,
      std::string_view ts_name = "ts",
      std::string_view dur_name = "dur");
  RelationWithColumns IntervalFillGaps(
      Relation input,
      ColumnId ts,
      ColumnId dur,
      base::Span<const ColumnId> keys,
      Relation background,
      ColumnId bg_ts,
      ColumnId bg_dur,
      base::Span<const ColumnId> bg_keys,
      base::Span<const FillGapsOutput> outputs);

  // Consumes this builder into a PreparedStatement outputting `outputs`.
  PreparedStatement Finish(Relation root,
                           base::Span<const NamedColumn> outputs);
  PreparedStatement Finish(Relation root, base::Span<const ColumnId> outputs);

 private:
  std::unique_ptr<LogicalPlan> plan_;
  uint32_t next_arg_index_ = 0;
  bool has_sql_scan_ = false;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_BUILDER_H_
