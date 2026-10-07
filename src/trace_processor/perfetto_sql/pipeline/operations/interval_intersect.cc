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
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"

namespace perfetto::trace_processor::pipeline {

const OperationRegistration IntervalIntersect::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION, &BuildPlan};

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
  // The region's bounds are the operator's own, so they are named before an
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
    // Every operand column is carried, reachable through the operand's alias.
    // A PER column holds the same value in every operand, so the first
    // operand's is also the one a bare name finds.
    Compiler::Alias operand_alias{*alias, {}};
    for (const NamedColumn& column : relation.columns) {
      bool is_key = std::find(operand.keys.begin(), operand.keys.end(),
                              column.id) != operand.keys.end();
      c->Append(column, source_id, /*qualified_only=*/!(is_key && i == 0));
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

}  // namespace perfetto::trace_processor::pipeline
