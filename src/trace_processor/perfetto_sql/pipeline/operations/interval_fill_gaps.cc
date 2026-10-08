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
#include "src/trace_processor/core/exec/assert_type.h"
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
    OperationRegistration::Encoding{4, false, &DecodePlan}};

namespace {

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

bool IsBounds(const std::string& name) {
  return base::CaseInsensitiveEqual(name, "ts") ||
         base::CaseInsensitiveEqual(name, "dur");
}

// The type both sides of a column are brought to: integers of any width are
// Int64. Nothing when a side's type is not known until it runs.
std::optional<core::StorageType> Normalized(
    const std::optional<core::StorageType>& type) {
  if (!type) {
    return std::nullopt;
  }
  if (type->Is<core::Id>() || type->Is<core::Uint32>() ||
      type->Is<core::Int32>() || type->Is<core::Int64>()) {
    return core::StorageType{core::Int64{}};
  }
  return type;
}

// A filler can make any column null, which an Id column cannot be; a Uint32
// holds the same values.
std::optional<core::StorageType> Nullable(
    std::optional<core::StorageType> type) {
  if (type && type->Is<core::Id>()) {
    return core::StorageType{core::Uint32{}};
  }
  return type;
}

}  // namespace

base::Status IntervalFillGaps::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("INTERVAL FILL GAPS");
  const auto* n = Node<SyntaqlitePerfettoIntervalFillGaps>(c->parser(), stage);

  IntervalFillGaps fill;
  ASSIGN_OR_RETURN(fill.ts_, c->Resolve("ts", stage));
  ASSIGN_OR_RETURN(fill.dur_, c->Resolve("dur", stage));
  std::vector<std::pair<std::string, uint32_t>> per;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* list =
        Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(list, i);
      std::string name = SpanText(
          c->parser(),
          Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
      RETURN_IF_ERROR(c->CheckListedOnce(seen, name, col_id));
      ASSIGN_OR_RETURN(ColumnId key, c->Resolve(name, col_id));
      fill.keys_.push_back(key);
      per.emplace_back(std::move(name), col_id);
    }
  }

  // The input is the plan so far; reading the background adds its scan.
  PlanNodeId input = c->plan().root();
  ASSIGN_OR_RETURN(SourceRelation background,
                   Scan::BuildRelation(c, n->background));
  std::optional<ColumnId> ts = FindColumn(background.columns, "ts");
  std::optional<ColumnId> dur = FindColumn(background.columns, "dur");
  if (!ts || !dur) {
    return c->Expected(n->background, std::string("the background to have a ") +
                                          (ts ? "dur" : "ts") + " column");
  }
  fill.background_ts_ = *ts;
  fill.background_dur_ = *dur;
  for (const auto& [name, col_id] : per) {
    if (std::optional<ColumnId> key = FindColumn(background.columns, name)) {
      fill.background_keys_.push_back(*key);
    }
  }
  if (!fill.background_keys_.empty() &&
      fill.background_keys_.size() != fill.keys_.size()) {
    return c->Expected(n->background,
                       "the background to have every PER column or none");
  }

  // Each column of the row takes the background's column of the same name on
  // fillers. A column only reachable through an alias names nothing there.
  const auto& types = c->plan().columns();
  std::vector<Compiler::RowColumn> row;
  std::vector<std::string> matched;
  for (const Compiler::RowColumn& column : c->row()) {
    Output output;
    output.input = column.column.id;
    std::optional<core::StorageType> type = types[column.column.id].type;
    bool bounds = column.column.id == fill.ts_ || column.column.id == fill.dur_;
    if (!column.qualified_only && !bounds && !IsBounds(column.column.name)) {
      output.background = FindColumn(background.columns, column.column.name);
    }
    if (output.background) {
      matched.push_back(column.column.name);
      auto in = Normalized(type);
      auto bg = Normalized(types[*output.background].type);
      if (in && bg && !(*in == *bg)) {
        return c->Expected(n->background,
                           "the background's '" + column.column.name +
                               "' to have the same type as the input's");
      }
      type = in ? in : bg;
    }
    output.column = c->AddColumn(column.column.name, Nullable(type));
    fill.outputs_.push_back(output);
    Compiler::RowColumn out = column;
    out.column.id = output.column;
    row.push_back(std::move(out));
  }
  // The background's other columns follow, null on input rows.
  for (const NamedColumn& column : background.columns) {
    bool taken =
        IsBounds(column.name) ||
        std::any_of(matched.begin(), matched.end(),
                    [&](const std::string& name) {
                      return base::CaseInsensitiveEqual(name, column.name);
                    });
    if (taken) {
      continue;
    }
    Output output;
    output.background = column.id;
    output.column = c->AddColumn(column.name, Nullable(types[column.id].type));
    fill.outputs_.push_back(output);
    row.push_back({{column.name, output.column}, c->operation(), stage, false});
  }
  // Fillers are rows the input's aliases never named.
  c->ReplaceRow(std::move(row));
  c->ClearAliases();
  c->AddNode(std::move(fill), {input, background.node});
  return base::OkStatus();
}

std::optional<uint32_t> IntervalFillGaps::Prune(std::vector<bool>* needed) {
  outputs_.erase(std::remove_if(outputs_.begin(), outputs_.end(),
                                [&](const Output& output) {
                                  return !(*needed)[output.column];
                                }),
                 outputs_.end());
  // The bounds and lanes on both sides decide where the fillers go.
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
    if (output.input) {
      (*needed)[*output.input] = true;
    }
    if (output.background) {
      (*needed)[*output.background] = true;
    }
  }
  return std::nullopt;
}

void IntervalFillGaps::Lower(Lowering* c, const PlanNode& node) const {
  const auto& fill = *this;
  c->LowerNode(node.children()[0]);
  const PlanNode& child = c->plan().nodes()[node.children()[1]];
  PERFETTO_DCHECK(child.Is<Scan>());
  const auto& scan = child.Cast<Scan>();
  auto position = [&](ColumnId column_id) {
    uint32_t at = 0;
    while (scan.columns()[at].id != column_id) {
      ++at;
    }
    return at;
  };

  // Both sides of a column agree on its type: an integer of any width is
  // widened, and a column of unknown type takes the other side's.
  std::vector<ex::Pipeline::Step> widen;
  c->RequireInt64(fill.ts_);
  c->RequireInt64(fill.dur_);
  c->RequireInt64(fill.background_ts_, position(fill.background_ts_), &widen);
  c->RequireInt64(fill.background_dur_, position(fill.background_dur_), &widen);
  for (const Output& output : fill.outputs_) {
    if (!output.input || !output.background) {
      continue;
    }
    const auto& types = c->plan().columns();
    auto type = Normalized(types[*output.input].type);
    if (!type) {
      type = Normalized(types[*output.background].type);
    }
    if (!type) {
      continue;
    }
    if (type->Is<core::Int64>()) {
      c->RequireInt64(*output.input);
      c->RequireInt64(*output.background, position(*output.background), &widen);
      continue;
    }
    ex::AssertTypeTarget target = type->Is<core::Double>()
                                      ? ex::AssertTypeTarget{core::Double{}}
                                      : ex::AssertTypeTarget{core::String{}};
    const auto& input_type = types[*output.input].type;
    if (!input_type || !(*input_type == *type)) {
      c->AddOperator(std::make_unique<ex::AssertType>(
          c->Position(*output.input), target, types[*output.input].name));
    }
    const auto& background_type = types[*output.background].type;
    if (!background_type || !(*background_type == *type)) {
      widen.push_back(
          std::make_unique<ex::AssertType>(position(*output.background), target,
                                           types[*output.background].name));
    }
  }

  ex::IntervalFillGapsSpec spec;
  spec.ts_column = c->Position(fill.ts_);
  spec.dur_column = c->Position(fill.dur_);
  for (ColumnId key : fill.keys_) {
    spec.key_columns.push_back(c->Position(key));
  }
  spec.background_ts_column = position(fill.background_ts_);
  spec.background_dur_column = position(fill.background_dur_);
  for (ColumnId key : fill.background_keys_) {
    spec.background_key_columns.push_back(position(key));
  }
  for (const Output& output : fill.outputs_) {
    ex::IntervalFillGapsSpec::Output lowered;
    if (output.input) {
      lowered.input = c->Position(*output.input);
    }
    if (output.background) {
      lowered.background = position(*output.background);
    }
    const auto& column = c->plan().columns()[output.column];
    lowered.fallback = column.type.value_or(core::StorageType{core::Int64{}});
    lowered.name = column.name;
    spec.outputs.push_back(std::move(lowered));
  }
  spec.background = c->AddOperand(scan.MakeSource(), std::move(widen));
  c->AddOperator(std::make_unique<ex::IntervalFillGaps>(std::move(spec)));

  // The fillers follow the input rows, so neither the layout nor any order
  // survives.
  c->ResetLayout();
  for (const Output& output : fill.outputs_) {
    c->Define(output.column);
  }
}

const OperationRegistration& IntervalFillGaps::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> IntervalFillGaps::Clone() const {
  return std::make_unique<IntervalFillGaps>(*this);
}

// Writes the background's scan inline, then replaces `available` with the
// result's columns.
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
  c->writer().U8(fill.background_keys_.empty() ? 0 : 1);
  for (ColumnId key : fill.background_keys_) {
    c->writer().Position(in, key);
  }
  c->writer().Size(fill.outputs_.size());
  for (const Output& output : fill.outputs_) {
    const auto& column = c->plan().columns()[output.column];
    c->writer().Str(column.name);
    WriteType(&c->writer(), column.type);
    c->writer().U8(static_cast<uint8_t>((output.input ? 1 : 0) |
                                        (output.background ? 2 : 0)));
    if (output.input) {
      c->writer().Position(available, *output.input);
    }
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
  switch (c->reader().U8()) {
    case 0:
      break;
    case 1:
      fill.background_keys_.resize(fill.keys_.size());
      for (ColumnId& key : fill.background_keys_) {
        key = c->reader().Position(in);
      }
      break;
    default:
      c->reader().Fail();
      return;
  }
  fill.outputs_.resize(c->reader().Count());
  for (Output& output : fill.outputs_) {
    std::string name = c->reader().Str();
    std::optional<core::StorageType> type = ReadType(&c->reader());
    uint8_t sides = c->reader().U8();
    if (sides == 0 || sides > 3) {
      c->reader().Fail();
      return;
    }
    if (sides & 1) {
      output.input = c->reader().Position(available);
    }
    if (sides & 2) {
      output.background = c->reader().Position(in);
    }
    output.column = c->AddColumn(std::move(name), type);
  }
  available.clear();
  for (const Output& output : fill.outputs_) {
    available.push_back(output.column);
  }
  c->AddNode(std::move(fill), {input, background});
}

}  // namespace perfetto::trace_processor::pipeline
