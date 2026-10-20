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
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalIntersect::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION, &BuildPlan,
    OperationRegistration::Encoding{2, true,
                                    &MakeOperation<IntervalIntersect>}};

namespace {

// The column of a relation named `name`, or nothing when it has none.
std::optional<ColumnId> FindColumn(const Compiler& c,
                                   const std::vector<NamedColumn>& columns,
                                   NameId name) {
  for (const NamedColumn& column : columns) {
    if (c.Same(column.name, name)) {
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
  NameId ts_name = c->Name("ts");
  NameId dur_name = c->Name("dur");
  isect.ts_ = c->AddColumn(ts_name, core::Int64{});
  isect.dur_ = c->AddColumn(dur_name, core::Int64{});

  c->Append({ts_name, isect.ts_}, node);
  c->Append({dur_name, isect.dur_}, node);
  std::vector<PlanNodeId> children;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t source_id = syntaqlite_list_child_id(list, i);
    const auto* source =
        Node<SyntaqlitePerfettoPipeSource>(c->parser(), source_id);
    std::optional<NameId> alias = Scan::SourceQualifier(c, *source);
    if (!alias) {
      return c->Expected(source_id,
                         "an alias for the relation, as in `(...) AS x`");
    }
    if (c->FindAlias(*alias)) {
      return c->Expected(source_id, "a different alias for each relation");
    }
    ASSIGN_OR_RETURN(SourceRelation relation,
                     Scan::BuildRelation(c, source_id));
    std::optional<ColumnId> ts = FindColumn(*c, relation.columns, ts_name);
    std::optional<ColumnId> dur = FindColumn(*c, relation.columns, dur_name);
    if (!ts || !dur) {
      return c->Expected(source_id, c->Text(*alias) + " to have a " +
                                        (ts ? "dur" : "ts") + " column");
    }
    IntervalIntersect::Operand operand;
    operand.ts = *ts;
    operand.dur = *dur;
    for (uint32_t k = 0; k < key_count; k++) {
      uint32_t col_id = syntaqlite_list_child_id(per, k);
      const auto* col = Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id);
      NameId name = c->Name(col->name);
      std::optional<ColumnId> key = FindColumn(*c, relation.columns, name);
      if (!key) {
        return c->Expected(col_id, c->Text(*alias) + " to have a " +
                                       c->Text(name) + " column");
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

  // Each operand is read from its own side pipeline.
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

void IntervalIntersect::Serialize(PlanCodec& c,
                                  std::vector<PlanNodeId>* children,
                                  Available* available) {
  c.NewColumn(&ts_, core::Int64{});
  c.NewColumn(&dur_, core::Int64{});
  operands_.resize(c.Count(operands_.size()));
  c.Check(operands_.size() >= 2);
  // Every operand has the same PER columns.
  uint32_t keys = c.Count(operands_.empty() ? 0 : operands_[0].keys.size());
  *available = {ts_, dur_};
  for (uint32_t k = 0; k < operands_.size(); ++k) {
    Operand& operand = operands_[k];
    // Each operand is a scan, written in place rather than as a stage.
    Scan read;
    Scan& scan =
        c.reading() ? read : c.plan().nodes()[(*children)[k]].Cast<Scan>();
    Available in;
    scan.Serialize(c, nullptr, &in);
    if (c.reading()) {
      children->push_back(c.plan().AddNode(std::move(read)));
    }
    c.Column(in, &operand.ts);
    c.Column(in, &operand.dur);
    operand.keys.resize(keys);
    for (ColumnId& id : operand.keys) {
      c.Column(in, &id);
    }
    c.Columns(in, &operand.carried);
    available->insert(available->end(), operand.carried.begin(),
                      operand.carried.end());
  }
}

}  // namespace perfetto::trace_processor::pipeline
