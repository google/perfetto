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
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration IntervalFillGaps::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_FILL_GAPS, &BuildPlan,
    OperationRegistration::Encoding{6, &MakeOperation<IntervalFillGaps>}};

namespace {

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
  return background ? CommonType(input, *background) : input;
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
  std::vector<ColumnId> keys;
  std::vector<NameId> per;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* list =
        Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(list, i);
      NameId name =
          c->Name(Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(name, col_id));
      // A PER column listed again adds nothing to the grouping.
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
        continue;
      }
      keys.push_back(key);
      per.push_back(name);
    }
  }

  // The input is the plan so far. The background is added as a new scan.
  PlanNodeId input = c->plan().root();
  ASSIGN_OR_RETURN(SourceRelation background,
                   Scan::BuildRelation(c, n->background));
  std::optional<ColumnId> ts = c->FindColumn(background.columns, ts_name);
  std::optional<ColumnId> dur = c->FindColumn(background.columns, dur_name);
  if (!ts || !dur) {
    return c->Expected(n->background, std::string("the background to have a ") +
                                          (ts ? "dur" : "ts") + " column");
  }
  fill.background_ts_ = *ts;
  fill.background_dur_ = *dur;
  std::vector<ColumnId> background_keys;
  for (NameId name : per) {
    if (std::optional<ColumnId> key = c->FindColumn(background.columns, name)) {
      background_keys.push_back(*key);
    }
  }
  if (!background_keys.empty() && background_keys.size() != keys.size()) {
    return c->Expected(n->background,
                       "the background to have every PER column or none");
  }

  // Fillers take each column from the background column with the same name.
  // Columns only reachable through an alias are not matched.
  const auto& types = c->plan().columns();
  std::vector<Compiler::RowColumn> row;
  std::vector<std::pair<ColumnId, ColumnId>> replaced;
  std::vector<ColumnId> matched;
  std::vector<Output> outputs;
  for (const Compiler::RowColumn& column : c->row()) {
    Output output;
    output.input = column.column.id;
    bool bounds = column.column.id == fill.ts_ || column.column.id == fill.dur_;
    if (!column.qualified_only && !bounds && !is_bounds(column.column.name)) {
      output.background = c->FindColumn(background.columns, column.column.name);
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
    outputs.push_back(output);
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
  fill.keys_ = c->arena().CopySpan<ColumnId>(keys);
  fill.background_keys_ = c->arena().CopySpan<ColumnId>(background_keys);
  fill.outputs_ = c->arena().CopySpan<Output>(outputs);
  c->AddNode(fill, {input, background.node});
  return base::OkStatus();
}

std::optional<uint32_t> IntervalFillGaps::Prune(std::vector<bool>* needed) {
  outputs_.RemoveIf(
      [&](const Output& output) { return !(*needed)[output.column]; });
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

InputNeeds IntervalFillGaps::Needs(uint32_t input) const {
  return input == 0 ? IntervalLaneNeeds(ts_, dur_, keys_.span())
                    : IntervalLaneNeeds(background_ts_, background_dur_,
                                        background_keys_.span());
}

std::vector<std::vector<InputColumn>> IntervalFillGaps::CommonTypes() const {
  std::vector<std::vector<InputColumn>> common;
  // Columns present on both sides are converted to a common type.
  for (const Output& output : outputs_) {
    if (output.background) {
      common.push_back({{0, output.input}, {1, *output.background}});
    }
  }
  // PER columns are numbered in one table, so both sides need one type even
  // if they are not output.
  for (uint32_t k = 0; k < background_keys_.size(); ++k) {
    common.push_back({{0, keys_[k]}, {1, background_keys_[k]}});
  }
  return common;
}

OutputProperties IntervalFillGaps::Derive(DeriveContext& c) const {
  // The output has no known order. Filler batches can be small.
  OutputProperties out;
  for (const Output& output : outputs_) {
    out.columns.push_back(c.ColumnOf(output.column));
  }
  out.full_batches = false;
  return out;
}

void IntervalFillGaps::Lower(Lowering* c, const PlanNode& node) const {
  const auto& fill = *this;
  c->LowerInput(node, 0);
  // The background is read from its own side pipeline.
  Lowering& background = c->AddSidePipeline();
  background.LowerInput(node, 1);

  ex::IntervalFillGapsSpec spec;
  spec.input = c->QueryIntervalLanes(fill.ts_, fill.dur_, fill.keys_.span());
  spec.input.forever = true;
  spec.background.lanes = background.QueryIntervalLanes(
      fill.background_ts_, fill.background_dur_, fill.background_keys_.span());
  core::Arena& arena = c->arena();
  std::vector<uint32_t> retained;
  spec.outputs = arena.NewSpan<ex::IntervalFillGapsSpec::Output>(
      static_cast<uint32_t>(fill.outputs_.size()));
  for (uint32_t i = 0; i < fill.outputs_.size(); ++i) {
    const Output& output = fill.outputs_[i];
    ex::IntervalFillGapsSpec::Output lowered;
    lowered.input = c->QueryPosition(output.input);
    if (output.background) {
      // Only the background columns fillers take are kept.
      lowered.background = static_cast<uint32_t>(retained.size());
      retained.push_back(background.QueryPosition(*output.background));
    }
    const auto& column = c->plan().columns()[output.column];
    lowered.fallback =
        column.type.storage().value_or(core::StorageType{core::Int64{}});
    lowered.name = arena.CopyString(c->plan().names().text(column.name));
    spec.outputs[i] = lowered;
  }
  spec.background.retained_columns = arena.CopySpan<uint32_t>(retained);
  spec.background.pipeline = background.FinishSide();
  c->AddOperator(arena.New<ex::IntervalFillGaps>(arena, spec));
}

const OperationRegistration& IntervalFillGaps::registration() const {
  return kRegistration;
}

void IntervalFillGaps::Serialize(PlanCodec& c) {
  c.Pod(&ts_);
  c.Pod(&dur_);
  c.Vec(&keys_);
  c.Pod(&background_ts_);
  c.Pod(&background_dur_);
  c.Vec(&background_keys_);
  c.Resize(&outputs_);
  PERFETTO_NO_VECTORIZE_LOOP
  for (Output& output : outputs_) {
    c.Pod(&output.column);
    c.Pod(&output.input);
    bool from_background = output.background.has_value();
    c.Pod(&from_background);
    ColumnId background = output.background.value_or(0);
    c.Pod(&background);
    if (from_background) {
      output.background = background;
    }
  }
}

}  // namespace perfetto::trace_processor::pipeline
