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

#include "src/trace_processor/perfetto_sql/pipeline/builder.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/aggregate.h"
#include "src/trace_processor/perfetto_sql/pipeline/column_pruning.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/aggregate_stage.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/compute.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/distinct.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_flatten.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/join.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/limit.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/order_by.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_key_paths.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {
namespace {

std::vector<ColumnId> AddAggregates(
    LogicalPlan* plan,
    base::Span<const Builder::AggregateItem> items,
    core::ArenaSpan<Aggregate>* out_aggs) {
  auto count = static_cast<uint32_t>(items.size());
  *out_aggs = plan->arena().NewSpan<Aggregate>(count);
  std::vector<ColumnId> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const Builder::AggregateItem& item = items[i];
    ColumnId col_id = plan->AddColumn(item.name, core::Int64{});
    Aggregate agg;
    agg.column = item.column;
    agg.output = col_id;
    agg.function = item.function;
    (*out_aggs)[i] = agg;
    out_cols.push_back(col_id);
  }
  return out_cols;
}

}  // namespace

Builder::Builder() : plan_(std::make_unique<LogicalPlan>()) {}
Builder::Builder(Builder&&) noexcept = default;
Builder& Builder::operator=(Builder&&) noexcept = default;
Builder::~Builder() = default;

NameId Builder::InternName(std::string_view text) {
  PERFETTO_CHECK(plan_ != nullptr);
  return plan_->names().Intern(text);
}

const std::string& Builder::NameText(NameId name) const {
  PERFETTO_CHECK(plan_ != nullptr);
  return plan_->names().text(name);
}

bool Builder::SameName(NameId a, NameId b) const {
  PERFETTO_CHECK(plan_ != nullptr);
  return plan_->names().Same(a, b);
}

PlanType Builder::ColumnType(ColumnId column) const {
  PERFETTO_CHECK(plan_ != nullptr);
  return plan_->columns()[column].type;
}

base::StatusOr<PlanType> Builder::CheckExprType(const Expr& expr) const {
  PERFETTO_CHECK(plan_ != nullptr);
  return pipeline::CheckExprType(*plan_, expr);
}

Literal Builder::StringLiteral(std::string_view text) {
  PERFETTO_CHECK(plan_ != nullptr);
  return Literal::String(plan_->arena().CopyString(text));
}

Expr Builder::MakeExpr(base::Span<const core::exec::ExprStep> nodes,
                       base::Span<const Literal> literals) {
  PERFETTO_CHECK(plan_ != nullptr);
  core::Arena& arena = plan_->arena();
  Expr expr;
  expr.nodes = arena.CopySpan<core::exec::ExprStep>(nodes);
  expr.literals = arena.CopySpan<Literal>(literals);
  for (Literal& lit : expr.literals) {
    if (lit.is_string()) {
      lit.text = arena.CopyString(lit.text);
    }
  }
  return expr;
}

Builder::ScanResult Builder::Scan(const core::dataframe::Dataframe& schema,
                                  base::Span<const uint32_t> column_indices) {
  PERFETTO_CHECK(plan_ != nullptr);
  PERFETTO_CHECK(!has_sql_scan_);
  core::Arena& arena = plan_->arena();
  auto count = static_cast<uint32_t>(column_indices.size());
  pipeline::Scan scan;
  scan.columns_ = arena.NewSpan<NamedColumn>(count);
  ScanDataframeArg source;
  source.index = next_arg_index_++;
  source.read = arena.CopySpan<uint32_t>(column_indices);
  std::vector<NamedColumn> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t df_col = column_indices[i];
    NameId name = plan_->names().Intern(schema.column_names()[df_col]);
    ColumnId col_id =
        plan_->AddColumn(name, Scan::ColumnTypeFor(schema, df_col));
    NamedColumn named{name, col_id};
    scan.columns_[i] = named;
    out_cols.push_back(named);
  }
  scan.source_ = source;
  PlanNodeId node = plan_->AddNode(scan);
  return {Relation{node}, std::move(out_cols)};
}

Builder::ScanResult Builder::Scan(const core::dataframe::Dataframe& schema) {
  std::vector<uint32_t> indices;
  const std::vector<std::string>& names = schema.column_names();
  indices.reserve(names.size());
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (!core::dataframe::IsHiddenColumn(names[i])) {
      indices.push_back(i);
    }
  }
  return Scan(schema, indices);
}

Builder::ScanResult Builder::ScanTable(const core::dataframe::Dataframe& schema,
                                       std::string_view table_name,
                                       base::Span<const TableColumn> columns) {
  PERFETTO_CHECK(plan_ != nullptr);
  core::Arena& arena = plan_->arena();
  auto count = static_cast<uint32_t>(columns.size());
  pipeline::Scan scan;
  scan.columns_ = arena.NewSpan<NamedColumn>(count);
  ScanDataframe source;
  source.name = arena.CopyString(table_name);
  source.read = arena.NewSpan<ScanDataframeColumn>(count);
  std::vector<NamedColumn> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const TableColumn& col = columns[i];
    NameId df_name = plan_->names().Intern(schema.column_names()[col.index]);
    ColumnId col_id =
        plan_->AddColumn(col.name, Scan::ColumnTypeFor(schema, col.index));
    NamedColumn named{col.name, col_id};
    scan.columns_[i] = named;
    source.read[i] = {col.index, df_name};
    out_cols.push_back(named);
  }
  scan.source_ = source;
  PlanNodeId node = plan_->AddNode(scan);
  return {Relation{node}, std::move(out_cols)};
}

Builder::ScanResult Builder::ScanSql(std::string_view sql,
                                     base::Span<const SqlColumn> columns) {
  PERFETTO_CHECK(plan_ != nullptr);
  PERFETTO_CHECK(next_arg_index_ == 0);
  has_sql_scan_ = true;
  core::Arena& arena = plan_->arena();
  auto count = static_cast<uint32_t>(columns.size());
  pipeline::Scan scan;
  scan.source_ = arena.CopyString(sql);
  scan.columns_ = arena.NewSpan<NamedColumn>(count);
  std::vector<NamedColumn> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const SqlColumn& col = columns[i];
    ColumnId col_id = plan_->AddColumn(col.name, col.type);
    NamedColumn named{col.name, col_id};
    scan.columns_[i] = named;
    out_cols.push_back(named);
  }
  PlanNodeId node = plan_->AddNode(scan);
  return {Relation{node}, std::move(out_cols)};
}

Builder::Relation Builder::Where(Relation input,
                                 std::vector<WhereCondition> conditions) {
  PERFETTO_CHECK(plan_ != nullptr);
  if (conditions.empty()) {
    return input;
  }
  core::Arena& arena = plan_->arena();
  uint32_t total_literals = 0;
  for (const WhereCondition& cond : conditions) {
    total_literals += static_cast<uint32_t>(cond.values.size());
  }
  pipeline::Where where;
  where.predicates_ =
      arena.NewSpan<WherePredicate>(static_cast<uint32_t>(conditions.size()));
  where.literals_ = arena.NewSpan<Literal>(total_literals);
  uint32_t lit_idx = 0;
  for (uint32_t i = 0; i < conditions.size(); ++i) {
    const WhereCondition& cond = conditions[i];
    where.predicates_[i] = {cond.column, cond.op, lit_idx,
                            static_cast<uint32_t>(cond.values.size())};
    for (Literal lit : cond.values) {
      if (lit.is_string()) {
        lit.text = arena.CopyString(lit.text);
      }
      where.literals_[lit_idx++] = lit;
    }
  }
  return Relation{plan_->AddNode(where, {input.id})};
}

Builder::RelationWithColumns Builder::Compute(Relation input,
                                              std::vector<ComputeItem> items) {
  PERFETTO_CHECK(plan_ != nullptr);
  if (items.empty()) {
    return {input, {}};
  }
  auto count = static_cast<uint32_t>(items.size());
  core::ArenaSpan<pipeline::Compute::Item> computed =
      plan_->arena().NewSpan<pipeline::Compute::Item>(count);
  std::vector<ColumnId> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const ComputeItem& item = items[i];
    ColumnId col_id = plan_->AddColumn(item.name, item.type);
    computed[i] = {col_id, item.expr};
    out_cols.push_back(col_id);
  }
  PlanNodeId node = plan_->AddNode(pipeline::Compute(computed), {input.id});
  return {Relation{node}, std::move(out_cols)};
}

Builder::Relation Builder::Distinct(Relation input,
                                    base::Span<const ColumnId> keys) {
  PERFETTO_CHECK(plan_ != nullptr);
  if (keys.empty()) {
    return input;
  }
  PERFETTO_CHECK(keys.size() <= core::exec::Distinct::kMaxColumns);
  pipeline::Distinct distinct;
  distinct.columns_ = plan_->arena().CopySpan<ColumnId>(keys);
  return Relation{plan_->AddNode(distinct, {input.id})};
}

Builder::RelationWithColumns Builder::Aggregate(
    Relation input,
    base::Span<const ColumnId> keys,
    base::Span<const AggregateItem> aggregates) {
  PERFETTO_CHECK(plan_ != nullptr);
  AggregateStage stage;
  stage.keys_ = plan_->arena().CopySpan<ColumnId>(keys);
  std::vector<ColumnId> out_cols =
      AddAggregates(plan_.get(), aggregates, &stage.aggregates_);
  PlanNodeId node = plan_->AddNode(stage, {input.id});
  return {Relation{node}, std::move(out_cols)};
}

Builder::Relation Builder::Join(Relation left,
                                Relation right,
                                core::exec::JoinSpec::Kind kind,
                                ColumnId probe_key,
                                ColumnId build_key,
                                base::Span<const ColumnId> outputs) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::Join join;
  join.left_ = kind == core::exec::JoinSpec::Kind::kLeft;
  join.probe_key_ = probe_key;
  join.build_key_ = build_key;
  join.outputs_ = plan_->arena().CopySpan<ColumnId>(outputs);
  return Relation{plan_->AddNode(join, {left.id, right.id})};
}

Builder::Relation Builder::OrderBy(Relation input, std::vector<SortKey> keys) {
  PERFETTO_CHECK(plan_ != nullptr);
  if (keys.empty()) {
    return input;
  }
  pipeline::OrderBy order;
  order.keys_ = plan_->arena().CopySpan<SortKey>(keys);
  return Relation{plan_->AddNode(order, {input.id})};
}

Builder::Relation Builder::Limit(Relation input,
                                 int64_t count,
                                 int64_t offset) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::Limit limit;
  limit.count_ = count;
  limit.offset_ = offset;
  return Relation{plan_->AddNode(limit, {input.id})};
}

Builder::RelationWithColumns Builder::TreeAccumulate(
    Relation input,
    TreeDirection direction,
    ColumnId node,
    ColumnId parent,
    base::Span<const AggregateItem> aggregates) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::TreeAccumulate acc;
  acc.direction_ = direction;
  acc.node_column_ = node;
  acc.parent_column_ = parent;
  std::vector<ColumnId> out_cols =
      AddAggregates(plan_.get(), aggregates, &acc.aggregates_);
  PlanNodeId node_id = plan_->AddNode(acc, {input.id});
  return {Relation{node_id}, std::move(out_cols)};
}

Builder::TreeKeyPathsResult Builder::TreeKeyPaths(
    Relation input,
    ColumnId node,
    ColumnId parent,
    base::Span<const ColumnId> keys,
    NameId path_name,
    NameId parent_path_name) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::TreeKeyPaths paths;
  paths.node_column_ = node;
  paths.parent_column_ = parent;
  paths.keys_ = plan_->arena().CopySpan<ColumnId>(keys);
  paths.path_ = plan_->AddColumn(path_name, core::Int64{});
  paths.parent_path_ = plan_->AddColumn(parent_path_name, core::Int64{});
  PlanNodeId node_id = plan_->AddNode(paths, {input.id});
  return {Relation{node_id}, paths.path_, paths.parent_path_};
}

Builder::GraphTreeResult Builder::GraphTree(
    core::exec::GraphTreeSpec::Kind kind,
    Relation nodes,
    ColumnId node,
    base::Span<const ColumnId> carried,
    Relation edges,
    ColumnId source,
    ColumnId target,
    Relation seeds,
    ColumnId seed,
    std::string_view parent_name) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::GraphTree tree;
  tree.kind_ = kind;
  tree.node_ = node;
  tree.carried_ = plan_->arena().CopySpan<ColumnId>(carried);
  tree.source_ = source;
  tree.target_ = target;
  tree.seed_ = seed;
  tree.parent_ =
      plan_->AddColumn(plan_->names().Intern(parent_name), core::Int64{});
  PlanNodeId node_id = plan_->AddNode(tree, {nodes.id, edges.id, seeds.id});
  return {Relation{node_id}, tree.parent_};
}

Builder::IntervalIntersectResult Builder::IntervalIntersect(
    const std::vector<IntervalOperand>& operands,
    std::string_view ts_name,
    std::string_view dur_name) {
  PERFETTO_CHECK(plan_ != nullptr);
  core::Arena& arena = plan_->arena();
  auto count = static_cast<uint32_t>(operands.size());
  pipeline::IntervalIntersect isect;
  isect.ts_ = plan_->AddColumn(plan_->names().Intern(ts_name), core::Int64{});
  isect.dur_ = plan_->AddColumn(plan_->names().Intern(dur_name), core::Int64{});
  isect.operands_ = arena.NewSpan<pipeline::IntervalIntersect::Operand>(count);
  std::vector<PlanNodeId> children;
  children.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const IntervalOperand& op = operands[i];
    isect.operands_[i] = {op.ts, op.dur, arena.CopySpan<ColumnId>(op.keys),
                          arena.CopySpan<ColumnId>(op.carried)};
    children.push_back(op.relation.id);
  }
  PlanNodeId node_id = plan_->AddNode(isect, children);
  return {Relation{node_id}, isect.ts_, isect.dur_};
}

Builder::IntervalFlattenResult Builder::IntervalFlatten(
    Relation input,
    ColumnId ts,
    ColumnId dur,
    base::Span<const ColumnId> keys,
    base::Span<const AggregateItem> aggregates,
    std::string_view ts_name,
    std::string_view dur_name) {
  PERFETTO_CHECK(plan_ != nullptr);
  pipeline::IntervalFlatten flatten;
  flatten.ts_ = ts;
  flatten.dur_ = dur;
  flatten.keys_ = plan_->arena().CopySpan<ColumnId>(keys);
  std::vector<ColumnId> agg_cols =
      AddAggregates(plan_.get(), aggregates, &flatten.aggregates_);
  flatten.out_ts_ =
      plan_->AddColumn(plan_->names().Intern(ts_name), core::Int64{});
  flatten.out_dur_ =
      plan_->AddColumn(plan_->names().Intern(dur_name), core::Int64{});
  PlanNodeId node_id = plan_->AddNode(flatten, {input.id});
  return {Relation{node_id}, flatten.out_ts_, flatten.out_dur_,
          std::move(agg_cols)};
}

Builder::RelationWithColumns Builder::IntervalFillGaps(
    Relation input,
    ColumnId ts,
    ColumnId dur,
    base::Span<const ColumnId> keys,
    Relation background,
    ColumnId bg_ts,
    ColumnId bg_dur,
    base::Span<const ColumnId> bg_keys,
    base::Span<const FillGapsOutput> outputs) {
  PERFETTO_CHECK(plan_ != nullptr);
  core::Arena& arena = plan_->arena();
  auto count = static_cast<uint32_t>(outputs.size());
  pipeline::IntervalFillGaps fill;
  fill.ts_ = ts;
  fill.dur_ = dur;
  fill.keys_ = arena.CopySpan<ColumnId>(keys);
  fill.background_ts_ = bg_ts;
  fill.background_dur_ = bg_dur;
  fill.background_keys_ = arena.CopySpan<ColumnId>(bg_keys);
  fill.outputs_ = arena.NewSpan<pipeline::IntervalFillGaps::Output>(count);
  std::vector<ColumnId> out_cols;
  out_cols.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const FillGapsOutput& out = outputs[i];
    ColumnId col_id = plan_->AddColumn(out.name, out.type);
    fill.outputs_[i] = {col_id, out.input, out.background};
    out_cols.push_back(col_id);
  }
  PlanNodeId node_id = plan_->AddNode(fill, {input.id, background.id});
  return {Relation{node_id}, std::move(out_cols)};
}

PreparedStatement Builder::Finish(Relation root,
                                  base::Span<const NamedColumn> outputs) {
  PERFETTO_CHECK(plan_ != nullptr);
  plan_->SetRoot(root.id);
  plan_->output().assign(outputs.begin(), outputs.end());
  for (const NamedColumn& out : outputs) {
    PERFETTO_CHECK(out.id < plan_->columns().size());
  }
  PruneColumns(*plan_);
  return PreparedStatement(std::move(plan_));
}

PreparedStatement Builder::Finish(Relation root,
                                  base::Span<const ColumnId> outputs) {
  PERFETTO_CHECK(plan_ != nullptr);
  plan_->SetRoot(root.id);
  plan_->output().clear();
  plan_->output().reserve(outputs.size());
  for (ColumnId col : outputs) {
    PERFETTO_CHECK(col < plan_->columns().size());
    plan_->output().push_back(NamedColumn{plan_->columns()[col].name, col});
  }
  PruneColumns(*plan_);
  return PreparedStatement(std::move(plan_));
}

}  // namespace perfetto::trace_processor::pipeline
