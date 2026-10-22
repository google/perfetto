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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_walk.h"

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
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/interval_walk.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalWalk::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_WALK, &BuildPlan,
    OperationRegistration::Encoding{9, &MakeOperation<IntervalWalk>}};

base::Status IntervalWalk::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("INTERVAL WALK");
  const auto* n = Node<SyntaqlitePerfettoIntervalWalk>(c->parser(), stage);

  IntervalWalk walk;
  NameId ts_name = c->Name("ts");
  NameId dur_name = c->Name("dur");
  auto is_bounds = [&](NameId name) {
    return c->Same(name, ts_name) || c->Same(name, dur_name);
  };
  ASSIGN_OR_RETURN(walk.in_ts_, c->Resolve(ts_name, stage));
  ASSIGN_OR_RETURN(walk.in_dur_, c->Resolve(dur_name, stage));

  const auto* per_list =
      Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
  const auto* step_list =
      Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->step);
  uint32_t per_count = syntaqlite_list_count(per_list);
  uint32_t step_count = syntaqlite_list_count(step_list);
  if (per_count != step_count) {
    return c->Expected(n->step, "the same number of TO columns as PER columns");
  }

  std::vector<std::pair<NameId, uint32_t>> per_cols;
  std::vector<std::pair<NameId, uint32_t>> step_cols;
  std::vector<NameId> seen_per;
  for (uint32_t i = 0; i < per_count; ++i) {
    uint32_t per_id = syntaqlite_list_child_id(per_list, i);
    NameId per_name =
        c->Name(Node<SyntaqlitePerfettoPerColumn>(c->parser(), per_id)->name);
    RETURN_IF_ERROR(c->CheckListedOnce(seen_per, per_name, per_id));
    ASSIGN_OR_RETURN(ColumnId key, c->Resolve(per_name, per_id));
    walk.keys_.push_back(key);
    per_cols.emplace_back(per_name, per_id);

    uint32_t step_id = syntaqlite_list_child_id(step_list, i);
    NameId step_name =
        c->Name(Node<SyntaqlitePerfettoPerColumn>(c->parser(), step_id)->name);
    step_cols.emplace_back(step_name, step_id);
  }

  PlanNodeId input = c->plan().root();
  const auto* edges_source =
      Node<SyntaqlitePerfettoPipeSource>(c->parser(), n->edges);
  ASSIGN_OR_RETURN(SourceRelation edges, Scan::BuildRelation(c, n->edges));

  std::optional<ColumnId> edges_ts = c->FindColumn(edges.columns, ts_name);
  std::optional<ColumnId> edges_dur = c->FindColumn(edges.columns, dur_name);
  if (!edges_ts || !edges_dur) {
    return c->Expected(n->edges, std::string("the edges to have a ") +
                                     (edges_ts ? "dur" : "ts") + " column");
  }
  walk.edges_ts_ = *edges_ts;
  walk.edges_dur_ = *edges_dur;

  for (const auto& [name, col_id] : per_cols) {
    std::optional<ColumnId> key = c->FindColumn(edges.columns, name);
    if (!key) {
      return c->Expected(col_id,
                         "the edges to have a " + c->Text(name) + " column");
    }
    walk.edges_keys_.push_back(*key);
  }
  for (const auto& [name, col_id] : step_cols) {
    std::optional<ColumnId> next_key = c->FindColumn(edges.columns, name);
    if (!next_key) {
      return c->Expected(col_id,
                         "the edges to have a " + c->Text(name) + " column");
    }
    walk.edges_next_keys_.push_back(*next_key);
  }

  walk.out_ts_ = c->AddColumn(ts_name, core::Int64{});
  walk.out_dur_ = c->AddColumn(dur_name, core::Int64{});

  std::vector<Compiler::RowColumn> row;
  row.push_back({{ts_name, walk.out_ts_}, c->operation(), stage, false});
  row.push_back({{dur_name, walk.out_dur_}, c->operation(), stage, false});

  for (const Compiler::RowColumn& column : c->row()) {
    if (std::find(walk.input_carried_.begin(), walk.input_carried_.end(),
                  column.column.id) == walk.input_carried_.end()) {
      walk.input_carried_.push_back(column.column.id);
    }
    bool shadowed =
        is_bounds(column.column.name) ||
        c->FindColumn(edges.columns, column.column.name).has_value();
    Compiler::RowColumn out = column;
    if (shadowed) {
      out.qualified_only = true;
    }
    row.push_back(std::move(out));
  }

  Compiler::Alias edge_alias{c->Name(""), {}};
  for (const NamedColumn& column : edges.columns) {
    walk.edges_carried_.push_back(column.id);
    bool is_bound = column.id == walk.edges_ts_ || column.id == walk.edges_dur_;
    row.push_back({column, c->operation(), n->edges, is_bound});
    edge_alias.columns.push_back(row.back());
  }

  if (std::optional<NameId> qualifier =
          Scan::SourceQualifier(c, *edges_source)) {
    edge_alias.name = *qualifier;
    if (syntaqlite_node_is_present(edges_source->alias)) {
      c->RemoveAlias(*qualifier);
      c->AddAlias(std::move(edge_alias));
    } else if (!c->FindAlias(*qualifier)) {
      c->AddAlias(std::move(edge_alias));
    }
  }

  c->ReplaceRow(std::move(row));
  c->AddNode(std::move(walk), {input, edges.node});
  return base::OkStatus();
}

std::optional<uint32_t> IntervalWalk::Prune(std::vector<bool>* needed) {
  input_carried_.erase(
      std::remove_if(input_carried_.begin(), input_carried_.end(),
                     [&](ColumnId id) { return !(*needed)[id]; }),
      input_carried_.end());
  edges_carried_.erase(
      std::remove_if(edges_carried_.begin(), edges_carried_.end(),
                     [&](ColumnId id) { return !(*needed)[id]; }),
      edges_carried_.end());

  for (ColumnId id : {in_ts_, in_dur_, edges_ts_, edges_dur_}) {
    (*needed)[id] = true;
  }
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  for (ColumnId key : edges_keys_) {
    (*needed)[key] = true;
  }
  for (ColumnId next_key : edges_next_keys_) {
    (*needed)[next_key] = true;
  }
  for (ColumnId id : input_carried_) {
    (*needed)[id] = true;
  }
  for (ColumnId id : edges_carried_) {
    (*needed)[id] = true;
  }
  return std::nullopt;
}

void IntervalWalk::Lower(Lowering* c, const PlanNode& node) const {
  const auto& walk = *this;
  c->LowerNode(node.children()[0]);
  Lowering& edges = c->AddSidePipeline();
  edges.LowerNode(node.children()[1]);

  for (size_t k = 0; k < walk.keys_.size(); ++k) {
    c->RequireCommonType({{c, walk.keys_[k]},
                          {&edges, walk.edges_keys_[k]},
                          {&edges, walk.edges_next_keys_[k]}});
  }

  ex::IntervalWalkSpec spec;
  spec.input = c->RequireIntervalLanes(walk.in_ts_, walk.in_dur_, walk.keys_);
  spec.edges.lanes = edges.RequireIntervalLanes(walk.edges_ts_, walk.edges_dur_,
                                                walk.edges_keys_);

  auto retain_edge_col = [&](ColumnId id) -> uint32_t {
    uint32_t pos = edges.QueryPosition(id);
    auto it = std::find(spec.edges.retained_columns.begin(),
                        spec.edges.retained_columns.end(), pos);
    if (it != spec.edges.retained_columns.end()) {
      return static_cast<uint32_t>(it - spec.edges.retained_columns.begin());
    }
    auto idx = static_cast<uint32_t>(spec.edges.retained_columns.size());
    spec.edges.retained_columns.push_back(pos);
    return idx;
  };

  for (ColumnId next_key : walk.edges_next_keys_) {
    spec.next_key_columns.push_back(retain_edge_col(next_key));
  }

  spec.outputs.push_back({ex::IntervalWalkSpec::Source::kTs, 0});
  spec.outputs.push_back({ex::IntervalWalkSpec::Source::kDur, 0});

  for (ColumnId id : walk.input_carried_) {
    auto idx = static_cast<uint32_t>(spec.input_retained_columns.size());
    spec.input_retained_columns.push_back(c->QueryPosition(id));
    spec.outputs.push_back({ex::IntervalWalkSpec::Source::kInput, idx});
  }
  for (ColumnId id : walk.edges_carried_) {
    uint32_t idx = retain_edge_col(id);
    spec.outputs.push_back({ex::IntervalWalkSpec::Source::kEdge, idx});
  }

  spec.edges.source = edges.FinishSide();
  c->AddOperator(std::make_unique<ex::IntervalWalk>(std::move(spec)));

  c->ResetLayout();
  c->DeclareColumn(walk.out_ts_);
  c->DeclareColumn(walk.out_dur_);
  for (ColumnId id : walk.input_carried_) {
    c->DeclareColumn(id);
  }
  for (ColumnId id : walk.edges_carried_) {
    c->DeclareColumn(id);
  }
  if (!walk.keys_.empty() &&
      std::all_of(walk.keys_.begin(), walk.keys_.end(), [&](ColumnId key) {
        return std::find(walk.input_carried_.begin(), walk.input_carried_.end(),
                         key) != walk.input_carried_.end();
      })) {
    c->DeclareGrouped(walk.keys_);
  }
  c->DeclareSmallBatches();
}

const OperationRegistration& IntervalWalk::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalWalk::Clone() const {
  return std::make_unique<IntervalWalk>(*this);
}

void IntervalWalk::Serialize(PlanCodec& c) {
  c.Pod(&out_ts_);
  c.Pod(&out_dur_);
  c.Pod(&in_ts_);
  c.Pod(&in_dur_);
  c.Vec(&keys_);
  c.Vec(&input_carried_);
  c.Pod(&edges_ts_);
  c.Pod(&edges_dur_);
  c.Vec(&edges_keys_);
  c.Vec(&edges_next_keys_);
  c.Vec(&edges_carried_);
}

}  // namespace perfetto::trace_processor::pipeline
