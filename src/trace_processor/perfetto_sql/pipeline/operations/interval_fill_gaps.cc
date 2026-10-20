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

#include "src/trace_processor/perfetto_sql/pipeline/operations/interval_fill_gaps.h"

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
#include "src/trace_processor/core/exec/interval_fill_gaps.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration IntervalFillGaps::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_FILL_GAPS, &BuildPlan,
    OperationRegistration::Encoding{6, false, &DecodePlan}};

namespace {

// The column named `name`, or nullopt if there is none.
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

// The type of an output column. ts and dur are Int64, since lowering converts
// them. Other columns get the type both sides convert to. Fillers can make
// any column null, and Id columns cannot hold nulls, so Id becomes Uint32.
// Returns nullopt if one side is a string and the other a number.
std::optional<PlanType> OutputType(const PlanType& input,
                                   const PlanType* background,
                                   bool bounds) {
  if (bounds) {
    return PlanType(core::Int64{});
  }
  std::optional<PlanType> type =
      background ? CommonType(input, *background) : input;
  if (type && type->Is<core::Id>()) {
    return PlanType(core::Uint32{});
  }
  return type;
}

}  // namespace

base::Status IntervalFillGaps::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("INTERVAL FILL GAPS");
  const auto* n = Node<SyntaqlitePerfettoIntervalFillGaps>(c->parser(), stage);

  IntervalFillGaps fill;
  NameId ts_name = c->Name("ts");
  NameId dur_name = c->Name("dur");
  auto is_bounds = [&](NameId name) {
    return c->Same(name, ts_name) || c->Same(name, dur_name);
  };
  ASSIGN_OR_RETURN(fill.ts_, c->Resolve(ts_name, stage));
  ASSIGN_OR_RETURN(fill.dur_, c->Resolve(dur_name, stage));
  std::vector<NameId> per;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* list =
        Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(list, i);
      NameId name =
          c->Name(Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(per, name, col_id));
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(name, col_id));
      fill.keys_.push_back(key);
    }
  }

  // The input is the plan so far. The background is added as a new scan.
  PlanNodeId input = c->plan().root();
  ASSIGN_OR_RETURN(SourceRelation background,
                   Scan::BuildRelation(c, n->background));
  std::optional<ColumnId> ts = FindColumn(*c, background.columns, ts_name);
  std::optional<ColumnId> dur = FindColumn(*c, background.columns, dur_name);
  if (!ts || !dur) {
    return c->Expected(n->background, std::string("the background to have a ") +
                                          (ts ? "dur" : "ts") + " column");
  }
  fill.background_ts_ = *ts;
  fill.background_dur_ = *dur;
  for (NameId name : per) {
    if (std::optional<ColumnId> key =
            FindColumn(*c, background.columns, name)) {
      fill.background_keys_.push_back(*key);
    }
  }
  if (!fill.background_keys_.empty() &&
      fill.background_keys_.size() != fill.keys_.size()) {
    return c->Expected(n->background,
                       "the background to have every PER column or none");
  }

  // Fillers take each column from the background column with the same name.
  // Columns only reachable through an alias are not matched.
  const auto& types = c->plan().columns();
  std::vector<Compiler::RowColumn> row;
  std::vector<std::pair<ColumnId, ColumnId>> replaced;
  std::vector<ColumnId> matched;
  for (const Compiler::RowColumn& column : c->row()) {
    Output output;
    output.input = column.column.id;
    bool bounds = column.column.id == fill.ts_ || column.column.id == fill.dur_;
    if (!column.qualified_only && !bounds && !is_bounds(column.column.name)) {
      output.background =
          FindColumn(*c, background.columns, column.column.name);
    }
    std::optional<PlanType> type = OutputType(
        types[output.input].type,
        output.background ? &types[*output.background].type : nullptr, bounds);
    if (!type) {
      return c->Expected(n->background,
                         "the background's '" + c->Text(column.column.name) +
                             "' to hold the same kind of values as the "
                             "input's");
    }
    if (output.background) {
      matched.push_back(*output.background);
    }
    output.column = c->AddColumn(column.column.name, *type);
    fill.outputs_.push_back(output);
    replaced.emplace_back(column.column.id, output.column);
    Compiler::RowColumn out = column;
    out.column.id = output.column;
    row.push_back(std::move(out));
  }
  for (const NamedColumn& column : background.columns) {
    if (!is_bounds(column.name) &&
        std::find(matched.begin(), matched.end(), column.id) == matched.end()) {
      return c->Expected(n->background, "the input to have the background's '" +
                                            c->Text(column.name) + "'");
    }
  }
  // Aliases still refer to their columns.
  c->ReplaceRow(std::move(row));
  c->ReplaceAliasColumns(replaced);
  c->AddNode(std::move(fill), {input, background.node});
  return base::OkStatus();
}

std::optional<uint32_t> IntervalFillGaps::Prune(std::vector<bool>* needed) {
  outputs_.erase(std::remove_if(outputs_.begin(), outputs_.end(),
                                [&](const Output& output) {
                                  return !(*needed)[output.column];
                                }),
                 outputs_.end());
  // ts, dur and the PER columns on both sides decide where the fillers go.
  for (ColumnId id : {ts_, dur_, background_ts_, background_dur_}) {
    (*needed)[id] = true;
  }
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  for (ColumnId key : background_keys_) {
    (*needed)[key] = true;
  }
  for (const Output& output : outputs_) {
    (*needed)[output.input] = true;
    if (output.background) {
      (*needed)[*output.background] = true;
    }
  }
  return std::nullopt;
}

void IntervalFillGaps::Lower(Lowering* c, const PlanNode& node) const {
  const auto& fill = *this;
  c->LowerNode(node.children()[0]);
  // The background is read from its own side pipeline.
  Lowering& background = c->AddSidePipeline();
  background.LowerNode(node.children()[1]);

  // Columns present on both sides are converted to a common type.
  for (const Output& output : fill.outputs_) {
    if (output.background) {
      c->RequireCommonType(
          {{c, output.input}, {&background, *output.background}});
    }
  }

  // PER columns are numbered in one table, so both sides need one type even
  // if they are not output.
  for (size_t k = 0; k < fill.background_keys_.size(); ++k) {
    c->RequireCommonType(
        {{c, fill.keys_[k]}, {&background, fill.background_keys_[k]}});
  }

  ex::IntervalFillGapsSpec spec;
  spec.input = c->RequireIntervalLanes(fill.ts_, fill.dur_, fill.keys_);
  spec.input.forever = true;
  spec.background.lanes = background.RequireIntervalLanes(
      fill.background_ts_, fill.background_dur_, fill.background_keys_);
  for (const Output& output : fill.outputs_) {
    ex::IntervalFillGapsSpec::Output lowered;
    lowered.input = c->QueryPosition(output.input);
    if (output.background) {
      // Only the background columns fillers take are kept.
      lowered.background =
          static_cast<uint32_t>(spec.background.retained_columns.size());
      spec.background.retained_columns.push_back(
          background.QueryPosition(*output.background));
    }
    const auto& column = c->plan().columns()[output.column];
    lowered.fallback =
        column.type.storage().value_or(core::StorageType{core::Int64{}});
    lowered.name = c->plan().names().text(column.name);
    spec.outputs.push_back(std::move(lowered));
  }
  spec.background.source = background.FinishSide();
  c->AddOperator(std::make_unique<ex::IntervalFillGaps>(std::move(spec)));

  // The output has a new layout and no known order. Filler batches can be
  // small.
  c->ResetLayout();
  for (const Output& output : fill.outputs_) {
    c->DeclareColumn(output.column);
  }
  c->DeclareSmallBatches();
}

const OperationRegistration& IntervalFillGaps::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalFillGaps::Clone() const {
  return std::make_unique<IntervalFillGaps>(*this);
}

// Writes the background's scan inline, then replaces `available` with the
// output columns.
void IntervalFillGaps::Write(PlanWriter* c,
                             const PlanNode& node,
                             Available* available_columns) const {
  auto& available = *available_columns;
  const auto& fill = *this;
  c->writer().Position(available, fill.ts_);
  c->writer().Position(available, fill.dur_);
  c->writer().Size(fill.keys_.size());
  for (ColumnId key : fill.keys_) {
    c->writer().Position(available, key);
  }
  const PlanNode& child = c->plan().nodes()[node.children()[1]];
  PERFETTO_CHECK(child.Is<Scan>());
  Available in;
  child.operation().Write(c, child, &in);
  c->writer().Position(in, fill.background_ts_);
  c->writer().Position(in, fill.background_dur_);
  c->writer().Size(fill.background_keys_.size());
  for (ColumnId key : fill.background_keys_) {
    c->writer().Position(in, key);
  }
  // An output's type is derived from its inputs, so only its name is written.
  c->writer().Size(fill.outputs_.size());
  for (const Output& output : fill.outputs_) {
    c->writer().U32(c->plan().columns()[output.column].name);
    c->writer().Position(available, output.input);
    c->writer().U8(output.background ? 1 : 0);
    if (output.background) {
      c->writer().Position(in, *output.background);
    }
  }
  available.clear();
  for (const Output& output : fill.outputs_) {
    available.push_back(output.column);
  }
}

void IntervalFillGaps::DecodePlan(PlanReader* c, Available* available_columns) {
  auto& available = *available_columns;
  PlanNodeId input = c->plan().root();
  IntervalFillGaps fill;
  fill.ts_ = c->reader().Position(available);
  fill.dur_ = c->reader().Position(available);
  fill.keys_.resize(c->reader().Count());
  for (ColumnId& key : fill.keys_) {
    key = c->reader().Position(available);
  }
  Scan scan;
  Available in = Scan::ReadPayload(c, &scan);
  if (!c->reader().ok()) {
    return;
  }
  PlanNodeId background = c->AddNode(std::move(scan));
  fill.background_ts_ = c->reader().Position(in);
  fill.background_dur_ = c->reader().Position(in);
  fill.background_keys_.resize(c->reader().Count());
  if (!fill.background_keys_.empty() &&
      fill.background_keys_.size() != fill.keys_.size()) {
    c->reader().Fail();
    return;
  }
  for (ColumnId& key : fill.background_keys_) {
    key = c->reader().Position(in);
  }
  const auto& types = c->plan().columns();
  fill.outputs_.resize(c->reader().Count());
  for (Output& output : fill.outputs_) {
    NameId name = c->Name();
    output.input = c->reader().Position(available);
    if (c->reader().U8()) {
      output.background = c->reader().Position(in);
    }
    std::optional<PlanType> type = OutputType(
        types[output.input].type,
        output.background ? &types[*output.background].type : nullptr,
        output.input == fill.ts_ || output.input == fill.dur_);
    if (!c->reader().ok() || !type) {
      c->reader().Fail();
      return;
    }
    output.column = c->AddColumn(name, *type);
  }
  available.clear();
  for (const Output& output : fill.outputs_) {
    available.push_back(output.column);
  }
  c->AddNode(std::move(fill), {input, background});
}

}  // namespace perfetto::trace_processor::pipeline
