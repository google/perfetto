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

#include "src/trace_processor/perfetto_sql/pipesql/stages.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"

namespace perfetto::trace_processor::pipesql {
namespace {

using pipeline::Builder;
using pipeline::ColumnId;
using pipeline::NamedColumn;
using pipeline::NameId;
using pipeline::PlanType;

std::optional<PlanType> FillGapsOutputType(const PlanType& input,
                                           const PlanType* background,
                                           bool bounds) {
  if (bounds) {
    return PlanType(core::Int64{});
  }
  return background ? pipeline::CommonType(input, *background) : input;
}

}  // namespace

base::Status TranslateIntervalIntersect(Translator* t, uint32_t node) {
  t->SetOperation("INTERVAL INTERSECTION");
  const auto* n =
      Node<SyntaqlitePerfettoIntervalIntersection>(t->parser(), node);
  const auto* list =
      Node<SyntaqlitePerfettoPipeSourceList>(t->parser(), n->operands);
  uint32_t count = syntaqlite_list_count(list);
  if (count < 2) {
    return t->Expected(node, "at least two relations to intersect");
  }
  const SyntaqlitePerfettoPerColumnList* per =
      syntaqlite_node_is_present(n->per)
          ? Node<SyntaqlitePerfettoPerColumnList>(t->parser(), n->per)
          : nullptr;
  std::vector<std::pair<NameId, uint32_t>> per_columns;
  for (uint32_t k = 0; per && k < syntaqlite_list_count(per); k++) {
    uint32_t col_id = syntaqlite_list_child_id(per, k);
    NameId name =
        t->Name(Node<SyntaqlitePerfettoPerColumn>(t->parser(), col_id)->name);
    if (std::none_of(per_columns.begin(), per_columns.end(),
                     [&](const auto& p) { return t->Same(p.first, name); })) {
      per_columns.emplace_back(name, col_id);
    }
  }

  NameId ts_name = t->Name("ts");
  NameId dur_name = t->Name("dur");

  std::vector<Builder::IntervalOperand> operands;
  struct OperandScope {
    NameId alias;
    uint32_t source_id;
    std::vector<NamedColumn> columns;
  };
  std::vector<OperandScope> scopes;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t source_id = syntaqlite_list_child_id(list, i);
    const auto* source =
        Node<SyntaqlitePerfettoPipeSource>(t->parser(), source_id);
    std::optional<NameId> alias = SourceQualifier(t, *source);
    if (!alias) {
      return t->Expected(source_id,
                         "an alias for the relation, as in `(...) AS x`");
    }
    if (std::any_of(scopes.begin(), scopes.end(), [&](const OperandScope& s) {
          return t->Same(s.alias, *alias);
        })) {
      return t->Expected(source_id, "a different alias for each relation");
    }
    ASSIGN_OR_RETURN(Builder::ScanResult relation,
                     TranslateSourceRelation(t, source_id));
    std::optional<ColumnId> ts = t->FindColumn(relation.columns, ts_name);
    std::optional<ColumnId> dur = t->FindColumn(relation.columns, dur_name);
    if (!ts || !dur) {
      return t->Expected(source_id, t->Text(*alias) + " to have a " +
                                        (ts ? "dur" : "ts") + " column");
    }
    Builder::IntervalOperand operand;
    operand.relation = relation.relation;
    operand.ts = *ts;
    operand.dur = *dur;
    for (const auto& [name, col_id] : per_columns) {
      std::optional<ColumnId> key = t->FindColumn(relation.columns, name);
      if (!key) {
        return t->Expected(col_id, t->Text(*alias) + " to have a " +
                                       t->Text(name) + " column");
      }
      operand.keys.push_back(*key);
    }
    operand.carried.reserve(relation.columns.size());
    for (const NamedColumn& column : relation.columns) {
      operand.carried.push_back(column.id);
    }
    operands.push_back(std::move(operand));
    scopes.push_back({*alias, source_id, std::move(relation.columns)});
  }

  auto built = t->builder().IntervalIntersect(operands);
  t->SetRoot(built.relation);
  t->Append({ts_name, built.ts}, node);
  t->Append({dur_name, built.dur}, node);
  for (uint32_t i = 0; i < scopes.size(); ++i) {
    const OperandScope& s = scopes[i];
    const Builder::IntervalOperand& op = operands[i];
    size_t first = t->row().size();
    for (const NamedColumn& column : s.columns) {
      bool is_key =
          std::find(op.keys.begin(), op.keys.end(), column.id) != op.keys.end();
      bool is_bound = column.id == op.ts || column.id == op.dur;
      t->Append(column, s.source_id,
                /*qualified_only=*/is_bound || (is_key && i != 0));
    }
    t->AddAlias(s.alias, first);
  }
  return base::OkStatus();
}

base::Status TranslateIntervalFlatten(Translator* t, uint32_t stage) {
  t->SetOperation("INTERVAL FLATTEN");
  const auto* n = Node<SyntaqlitePerfettoIntervalFlatten>(t->parser(), stage);

  NameId ts = t->Name("ts");
  NameId dur = t->Name("dur");
  ASSIGN_OR_RETURN(ColumnId ts_col, t->Resolve(ts, stage));
  ASSIGN_OR_RETURN(ColumnId dur_col, t->Resolve(dur, stage));

  std::vector<ColumnId> per_keys;
  std::vector<std::pair<NamedColumn, uint32_t>> keys;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* per =
        Node<SyntaqlitePerfettoPerColumnList>(t->parser(), n->per);
    for (uint32_t i = 0; i < syntaqlite_list_count(per); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(per, i);
      NameId name =
          t->Name(Node<SyntaqlitePerfettoPerColumn>(t->parser(), col_id)->name);
      ASSIGN_OR_RETURN(ColumnId key, t->Resolve(name, col_id));
      if (std::find(per_keys.begin(), per_keys.end(), key) == per_keys.end()) {
        per_keys.push_back(key);
      }
      keys.push_back({NamedColumn{name, key}, col_id});
    }
  }

  ASSIGN_OR_RETURN(Translator::TranslatedAggregates aggs,
                   t->TranslateAggregateList(n->aggregates));
  auto built = t->builder().IntervalFlatten(t->root(), ts_col, dur_col,
                                            per_keys, aggs.items);
  t->SetRoot(built.relation);

  t->ReplaceRow({});
  t->ClearAliases();
  t->Append({ts, built.ts}, stage);
  t->Append({dur, built.dur}, stage);
  for (auto& [column, node] : keys) {
    t->Append(column, node);
  }
  for (size_t i = 0; i < aggs.items.size(); ++i) {
    t->Append(NamedColumn{aggs.items[i].name, built.aggregates[i]},
              aggs.nodes[i]);
  }
  return base::OkStatus();
}

base::Status TranslateIntervalFillGaps(Translator* t, uint32_t stage) {
  t->SetOperation("INTERVAL FILL GAPS");
  const auto* n = Node<SyntaqlitePerfettoIntervalFillGaps>(t->parser(), stage);

  NameId ts_name = t->Name("ts");
  NameId dur_name = t->Name("dur");
  auto is_bounds = [&](NameId name) {
    return t->Same(name, ts_name) || t->Same(name, dur_name);
  };
  ASSIGN_OR_RETURN(ColumnId ts_col, t->Resolve(ts_name, stage));
  ASSIGN_OR_RETURN(ColumnId dur_col, t->Resolve(dur_name, stage));
  std::vector<ColumnId> keys;
  std::vector<NameId> per;
  if (syntaqlite_node_is_present(n->per)) {
    const auto* list =
        Node<SyntaqlitePerfettoPerColumnList>(t->parser(), n->per);
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t col_id = syntaqlite_list_child_id(list, i);
      NameId name =
          t->Name(Node<SyntaqlitePerfettoPerColumn>(t->parser(), col_id)->name);
      ASSIGN_OR_RETURN(ColumnId key, t->Resolve(name, col_id));
      if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
        continue;
      }
      keys.push_back(key);
      per.push_back(name);
    }
  }

  Builder::Relation input = t->root();
  ASSIGN_OR_RETURN(Builder::ScanResult background,
                   TranslateSourceRelation(t, n->background));
  std::optional<ColumnId> bg_ts = t->FindColumn(background.columns, ts_name);
  std::optional<ColumnId> bg_dur = t->FindColumn(background.columns, dur_name);
  if (!bg_ts || !bg_dur) {
    return t->Expected(n->background, std::string("the background to have a ") +
                                          (bg_ts ? "dur" : "ts") + " column");
  }
  std::vector<ColumnId> bg_keys;
  for (NameId name : per) {
    if (std::optional<ColumnId> key = t->FindColumn(background.columns, name)) {
      bg_keys.push_back(*key);
    }
  }
  if (!bg_keys.empty() && bg_keys.size() != keys.size()) {
    return t->Expected(n->background,
                       "the background to have every PER column or none");
  }

  std::vector<Builder::FillGapsOutput> outputs;
  std::vector<ColumnId> matched;
  for (const Translator::RowColumn& column : t->row()) {
    ColumnId input_id = column.column.id;
    bool bounds = input_id == ts_col || input_id == dur_col;
    std::optional<ColumnId> bg_id;
    if (!column.qualified_only && !bounds && !is_bounds(column.column.name)) {
      bg_id = t->FindColumn(background.columns, column.column.name);
    }
    PlanType bg_type;
    if (bg_id) {
      bg_type = t->ColumnType(*bg_id);
    }
    std::optional<PlanType> type = FillGapsOutputType(
        t->ColumnType(input_id), bg_id ? &bg_type : nullptr, bounds);
    if (!type) {
      return t->Expected(n->background,
                         "the background's '" + t->Text(column.column.name) +
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
      return t->Expected(n->background, "the input to have the background's '" +
                                            t->Text(column.name) + "'");
    }
  }
  auto built = t->builder().IntervalFillGaps(input, ts_col, dur_col, keys,
                                             background.relation, *bg_ts,
                                             *bg_dur, bg_keys, outputs);
  t->SetRoot(built.relation);

  std::vector<Translator::RowColumn> row;
  std::vector<std::pair<ColumnId, ColumnId>> replaced;
  row.reserve(t->row().size());
  for (size_t i = 0; i < t->row().size(); ++i) {
    Translator::RowColumn out = t->row()[i];
    replaced.emplace_back(out.column.id, built.columns[i]);
    out.column.id = built.columns[i];
    row.push_back(std::move(out));
  }
  t->ReplaceRow(std::move(row));
  t->ReplaceAliasColumns(replaced);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
