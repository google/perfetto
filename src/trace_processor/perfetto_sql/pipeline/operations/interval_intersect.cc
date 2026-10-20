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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_intersect.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/interval_intersect.h"
#include "src/trace_processor/core/exec/interval_lanes.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/cost_estimation.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalIntersect::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION, &BuildPlan,
    OperationRegistration::Encoding{2, true, &DecodePlan}};

namespace {

// The position of `id` in `ids`, if it is there.
std::optional<size_t> Find(const std::vector<ColumnId>& ids, ColumnId id) {
  auto it = std::find(ids.begin(), ids.end(), id);
  return it == ids.end() ? std::nullopt
                         : std::make_optional<size_t>(
                               static_cast<size_t>(it - ids.begin()));
}

// The node of the scan an operand at `id` reads, and the predicates of the
// WHERE on what it reads, if any.
std::pair<const PlanNode*, const std::vector<WherePredicate>*> OperandScan(
    const LogicalPlan& plan,
    PlanNodeId id) {
  static const std::vector<WherePredicate>* const kNone =
      new std::vector<WherePredicate>();
  const PlanNode& node = plan.nodes()[id];
  if (node.Is<Where>()) {
    const PlanNode& scan = plan.nodes()[node.children()[0]];
    PERFETTO_CHECK(scan.Is<Scan>());
    return {&scan, &node.Cast<Where>().predicates()};
  }
  PERFETTO_CHECK(node.Is<Scan>());
  return {&node, kNone};
}

// The column of a relation named `name`, or nothing when it has none.
std::optional<ColumnId> FindColumn(const std::vector<NamedColumn>& columns,
                                   const std::string& name) {
  for (const NamedColumn& column : columns) {
    if (base::CaseInsensitiveEqual(column.name, name)) {
      return column.id;
    }
  }
  return std::nullopt;
}

}  // namespace

base::Status IntervalIntersect::BuildPlan(Compiler* c, uint32_t node) {
  c->SetOperation("INTERVAL INTERSECTION");
  const auto* n =
      Node<SyntaqlitePerfettoIntervalIntersection>(c->parser(), node);
  const auto* list =
      Node<SyntaqlitePerfettoPipeSourceList>(c->parser(), n->operands);
  uint32_t count = syntaqlite_list_count(list);
  if (count < 2) {
    return c->Expected(node, "at least two relations to intersect");
  }
  const SyntaqlitePerfettoPerColumnList* per =
      syntaqlite_node_is_present(n->per)
          ? Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per)
          : nullptr;
  uint32_t key_count = per ? syntaqlite_list_count(per) : 0;

  IntervalIntersect isect;
  // The region's bounds are the operation's own, so they are named before an
  // operand can bind anything.
  isect.ts_ = c->AddColumn("ts", core::Int64{});
  isect.dur_ = c->AddColumn("dur", core::Int64{});

  c->Append({"ts", isect.ts_}, node);
  c->Append({"dur", isect.dur_}, node);
  std::vector<PlanNodeId> children;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t source_id = syntaqlite_list_child_id(list, i);
    const auto* source =
        Node<SyntaqlitePerfettoPipeSource>(c->parser(), source_id);
    std::optional<std::string> alias = Scan::SourceQualifier(c, *source);
    if (!alias) {
      return c->Expected(source_id,
                         "an alias for the relation, as in `(...) AS x`");
    }
    if (c->FindAlias(*alias)) {
      return c->Expected(source_id, "a different alias for each relation");
    }
    ASSIGN_OR_RETURN(SourceRelation relation,
                     Scan::BuildRelation(c, source_id));
    std::optional<ColumnId> ts = FindColumn(relation.columns, "ts");
    std::optional<ColumnId> dur = FindColumn(relation.columns, "dur");
    if (!ts || !dur) {
      return c->Expected(
          source_id, *alias + " to have a " + (ts ? "dur" : "ts") + " column");
    }
    IntervalIntersect::Operand operand;
    operand.ts = *ts;
    operand.dur = *dur;
    for (uint32_t k = 0; k < key_count; k++) {
      uint32_t col_id = syntaqlite_list_child_id(per, k);
      const auto* col = Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id);
      std::string name = SpanText(c->parser(), col->name);
      std::optional<ColumnId> key = FindColumn(relation.columns, name);
      if (!key) {
        return c->Expected(col_id, *alias + " to have a " + name + " column");
      }
      operand.keys.push_back(*key);
    }
    // Every operand column is carried, reachable through the operand's alias
    // and, for most, by its bare name too. The region's ts and dur are what
    // a bare ts and dur mean, so an operand's own are found only through its
    // alias. A PER column holds the same value in every operand, so only the
    // first operand's is found by its bare name.
    Compiler::Alias operand_alias{*alias, {}};
    for (const NamedColumn& column : relation.columns) {
      bool is_key = std::find(operand.keys.begin(), operand.keys.end(),
                              column.id) != operand.keys.end();
      bool is_bound = column.id == operand.ts || column.id == operand.dur;
      c->Append(column, source_id,
                /*qualified_only=*/is_bound || (is_key && i != 0));
      operand_alias.columns.push_back(c->row().back());
      operand.carried.push_back(column.id);
    }
    c->AddAlias(std::move(operand_alias));
    children.push_back(relation.node);
    isect.operands_.push_back(std::move(operand));
  }
  c->AddNode(std::move(isect), std::move(children));
  return base::OkStatus();
}

std::optional<uint32_t> IntervalIntersect::Prune(std::vector<bool>* needed) {
  // An intersection passes on only the operand columns used after it, but
  // reads each operand's bounds and partition columns to find the regions.
  auto& isect = *this;
  for (IntervalIntersect::Operand& operand : isect.operands_) {
    auto& carried = operand.carried;
    carried.erase(std::remove_if(carried.begin(), carried.end(),
                                 [&](ColumnId id) { return !(*needed)[id]; }),
                  carried.end());
  }
  for (const IntervalIntersect::Operand& operand : isect.operands_) {
    (*needed)[operand.ts] = true;
    (*needed)[operand.dur] = true;
    for (ColumnId key : operand.keys) {
      (*needed)[key] = true;
    }
  }
  return std::nullopt;
}

// A region carries the columns of one row of each operand, so dropping the
// row drops exactly the regions a predicate on them would be tested in. A PER
// column holds the same value in every operand, so a predicate on one reaches
// all of them. The region's own bounds belong to no operand.
std::vector<std::vector<WherePredicate>> IntervalIntersect::PassFilters(
    const PlanNode&,
    std::vector<WherePredicate>* predicates) const {
  std::vector<std::vector<WherePredicate>> passed(operands_.size());
  std::vector<WherePredicate> kept;
  for (const WherePredicate& predicate : *predicates) {
    bool pass = false;
    for (size_t k = 0; k < operands_.size() && !pass; ++k) {
      const Operand& operand = operands_[k];
      if (std::optional<size_t> key = Find(operand.keys, predicate.column)) {
        for (size_t j = 0; j < operands_.size(); ++j) {
          WherePredicate copy = predicate;
          copy.column = operands_[j].keys[*key];
          passed[j].push_back(copy);
        }
        pass = true;
      } else if (Find(operand.carried, predicate.column)) {
        passed[k].push_back(predicate);
        pass = true;
      }
    }
    if (!pass) {
      kept.push_back(predicate);
    }
  }
  *predicates = std::move(kept);
  return passed;
}

core::RowEstimate IntervalIntersect::Estimate(PlanEstimator* e,
                                              const PlanNode& node) const {
  // Each operand is sorted, then swept together. A sweep produces about one
  // region per row it passes, though regions can multiply where operands
  // overlap themselves.
  double estimated = 0;
  double max = 1;
  for (PlanNodeId child : node.children()) {
    core::RowEstimate in = e->EstimateNode(child);
    e->AddSortCost(in.estimated);
    e->AddCost(PlanEstimator::kSweepCostPerRow * in.estimated);
    estimated += in.estimated;
    max *= std::max<uint32_t>(in.max, 1);
  }
  return {PlanEstimator::Rows(max), PlanEstimator::Rows(estimated)};
}

void IntervalIntersect::Lower(Lowering* c, const PlanNode& node) const {
  // Each operand runs as its own pipeline, so the intersection lowers
  // its children itself.
  const auto& isect = *this;
  const auto& children = node.children();

  PERFETTO_DCHECK(!c->has_source());
  PERFETTO_DCHECK(isect.operands_.size() == children.size());
  // The region's bounds come first, then each operand's carried columns in
  // turn.
  c->DeclareColumn(isect.ts_);
  c->DeclareColumn(isect.dur_);

  // Each operand is read from its own side pipeline, which also runs the
  // WHERE passed down to it, if any.
  std::vector<Lowering*> sides;
  for (uint32_t i = 0; i < isect.operands_.size(); i++) {
    Lowering& side = c->AddSidePipeline();
    side.LowerNode(children[i]);
    sides.push_back(&side);
  }

  // Keys are compared across operands, so each key must have the same type
  // in all of them.
  size_t key_count =
      isect.operands_.empty() ? 0 : isect.operands_[0].keys.size();
  for (size_t k = 0; k < key_count; ++k) {
    std::vector<Lowering::Side> same;
    for (uint32_t i = 0; i < isect.operands_.size(); i++) {
      same.push_back({sides[i], isect.operands_[i].keys[k]});
    }
    c->RequireCommonType(same);
  }

  std::vector<ex::CollectedInputSpec> inputs;
  for (uint32_t i = 0; i < isect.operands_.size(); i++) {
    const IntervalIntersect::Operand& operand = isect.operands_[i];
    Lowering& side = *sides[i];
    ex::CollectedInputSpec lowered;
    lowered.lanes =
        side.RequireIntervalLanes(operand.ts, operand.dur, operand.keys);
    for (ColumnId id : operand.carried) {
      lowered.retained_columns.push_back(side.QueryPosition(id));
    }
    lowered.source = side.FinishSide();
    inputs.push_back(std::move(lowered));

    for (ColumnId id : operand.carried) {
      c->DeclareColumn(id);
    }
  }

  // The output is grouped by the keys. This can only be declared if every
  // key is carried to the output by some operand.
  std::vector<ColumnId> carried_keys;
  for (size_t k = 0; k < key_count; ++k) {
    for (const IntervalIntersect::Operand& operand : isect.operands_) {
      const auto& carried = operand.carried;
      if (std::find(carried.begin(), carried.end(), operand.keys[k]) !=
          carried.end()) {
        carried_keys.push_back(operand.keys[k]);
        break;
      }
    }
  }
  if (key_count > 0 && carried_keys.size() == key_count) {
    c->DeclareGrouped(carried_keys);
  }
  c->SetSource(std::make_unique<ex::IntervalIntersect>(std::move(inputs)));
}

const OperationRegistration& IntervalIntersect::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalIntersect::Clone() const {
  return std::make_unique<IntervalIntersect>(*this);
}

void IntervalIntersect::Write(PlanWriter* c,
                              const PlanNode& node,
                              Available* available_columns) const {
  auto& available = *available_columns;
  const auto& isect = node.Cast<IntervalIntersect>();
  c->writer().Str(c->plan().columns()[isect.ts_].name);
  c->writer().Str(c->plan().columns()[isect.dur_].name);
  c->writer().Size(isect.operands_.size());
  c->writer().Size(isect.operands_.empty() ? 0
                                           : isect.operands_[0].keys.size());
  available = {isect.ts_, isect.dur_};
  for (uint32_t k = 0; k < isect.operands_.size(); ++k) {
    const IntervalIntersect::Operand& operand = isect.operands_[k];
    // An operand is a scan, written inline, then the WHERE on it.
    const auto [scan, predicates] = OperandScan(c->plan(), node.children()[k]);
    Available in;
    scan->operation().Write(c, *scan, &in);
    Where::WritePredicates(c, *predicates, in);
    c->writer().Position(in, operand.ts);
    c->writer().Position(in, operand.dur);
    for (ColumnId id : operand.keys) {
      c->writer().Position(in, id);
    }
    c->writer().Size(operand.carried.size());
    for (ColumnId id : operand.carried) {
      c->writer().Position(in, id);
      available.push_back(id);
    }
  }
}

void IntervalIntersect::DecodePlan(PlanReader* c,
                                   Available* available_columns) {
  auto& available = *available_columns;
  IntervalIntersect isect;
  isect.ts_ = c->AddColumn(c->reader().Str(), core::Int64{});
  isect.dur_ = c->AddColumn(c->reader().Str(), core::Int64{});
  available = {isect.ts_, isect.dur_};
  isect.operands_.resize(c->reader().Count());
  uint32_t keys = c->reader().Count();
  if (isect.operands_.size() < 2) {
    c->reader().Fail();
  }
  std::vector<PlanNodeId> children;
  for (IntervalIntersect::Operand& operand : isect.operands_) {
    Scan scan;
    Available in = Scan::ReadPayload(c, &scan);
    std::vector<WherePredicate> predicates = Where::ReadPredicates(c, in);
    if (!c->reader().ok()) {
      return;
    }
    PlanNodeId child = c->AddNode(std::move(scan));
    if (!predicates.empty()) {
      child = c->AddNode(Where(std::move(predicates)), {child});
    }
    children.push_back(child);
    operand.ts = c->reader().Position(in);
    operand.dur = c->reader().Position(in);
    operand.keys.resize(keys);
    for (ColumnId& id : operand.keys) {
      id = c->reader().Position(in);
    }
    operand.carried.resize(c->reader().Count());
    for (ColumnId& id : operand.carried) {
      id = c->reader().Position(in);
      available.push_back(id);
    }
  }
  c->AddNode(std::move(isect), std::move(children));
}

}  // namespace perfetto::trace_processor::pipeline
