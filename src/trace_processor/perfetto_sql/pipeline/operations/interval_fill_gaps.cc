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

  NameId ts_name = c->Name("ts");
  NameId dur_name = c->Name("dur");
  auto is_bounds = [&](NameId name) {
    return c->Same(name, ts_name) || c->Same(name, dur_name);
  };
  ASSIGN_OR_RETURN(ColumnId ts_col, c->Resolve(ts_name, stage));
  ASSIGN_OR_RETURN(ColumnId dur_col, c->Resolve(dur_name, stage));
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
  Builder::Relation input = c->root();
  ASSIGN_OR_RETURN(Builder::ScanResult background,
                   Scan::BuildRelation(c, n->background));
  std::optional<ColumnId> bg_ts = c->FindColumn(background.columns, ts_name);
  std::optional<ColumnId> bg_dur = c->FindColumn(background.columns, dur_name);
  if (!bg_ts || !bg_dur) {
    return c->Expected(n->background, std::string("the background to have a ") +
                                          (bg_ts ? "dur" : "ts") + " column");
  }
  std::vector<ColumnId> bg_keys;
  for (NameId name : per) {
    if (std::optional<ColumnId> key = c->FindColumn(background.columns, name)) {
      bg_keys.push_back(*key);
    }
  }
  if (!bg_keys.empty() && bg_keys.size() != keys.size()) {
    return c->Expected(n->background,
                       "the background to have every PER column or none");
  }

  // Fillers take each column from the background column with the same name.
  // Columns only reachable through an alias are not matched.
  std::vector<Builder::FillGapsOutput> outputs;
  std::vector<ColumnId> matched;
  for (const Compiler::RowColumn& column : c->row()) {
    ColumnId input_id = column.column.id;
    bool bounds = input_id == ts_col || input_id == dur_col;
    std::optional<ColumnId> bg_id;
    if (!column.qualified_only && !bounds && !is_bounds(column.column.name)) {
      bg_id = c->FindColumn(background.columns, column.column.name);
    }
    PlanType bg_type;
    if (bg_id) {
      bg_type = c->ColumnType(*bg_id);
    }
    std::optional<PlanType> type =
        OutputType(c->ColumnType(input_id), bg_id ? &bg_type : nullptr, bounds);
    if (!type) {
      return c->Expected(n->background,
                         "the background's '" + c->Text(column.column.name) +
                             "' to hold the same kind of values as the "
                             "input's");
    }
    if (bg_id) {
      matched.push_back(*bg_id);
    }
    outputs.push_back({column.column.name, *type, input_id, bg_id});
  }
  for (const NamedColumn& column : background.columns) {
    if (!is_bounds(column.name) &&
        std::find(matched.begin(), matched.end(), column.id) == matched.end()) {
      return c->Expected(n->background, "the input to have the background's '" +
                                            c->Text(column.name) + "'");
    }
  }
  auto built = c->builder().IntervalFillGaps(input, ts_col, dur_col, keys,
                                             background.relation, *bg_ts,
                                             *bg_dur, bg_keys, outputs);
  c->SetRoot(built.relation);

  std::vector<Compiler::RowColumn> row;
  std::vector<std::pair<ColumnId, ColumnId>> replaced;
  row.reserve(c->row().size());
  for (size_t i = 0; i < c->row().size(); ++i) {
    Compiler::RowColumn out = c->row()[i];
    replaced.emplace_back(out.column.id, built.columns[i]);
    out.column.id = built.columns[i];
    row.push_back(std::move(out));
  }
  // Aliases still refer to their columns.
  c->ReplaceRow(std::move(row));
  c->ReplaceAliasColumns(replaced);
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
