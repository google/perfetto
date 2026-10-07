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

#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_accumulate.h"

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
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"

namespace perfetto::trace_processor::pipeline {

const OperationRegistration TreeAccumulate::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE, &BuildPlan};

base::Status TreeAccumulate::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("TREE ACCUMULATE");
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(c->parser(), stage);

  TreeAccumulate acc;
  acc.direction_ = n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
                       ? TreeDirection::kDown
                       : TreeDirection::kUp;
  ASSIGN_OR_RETURN(acc.node_column_, c->Resolve("id", stage));
  ASSIGN_OR_RETURN(acc.parent_column_, c->Resolve("parent_id", stage));

  std::vector<std::pair<NamedColumn, uint32_t>> output;
  const auto* list =
      Node<SyntaqlitePerfettoAggregateList>(c->parser(), n->aggregates);
  uint32_t count = syntaqlite_list_count(list);
  for (uint32_t i = 0; i < count; i++) {
    uint32_t agg_id = syntaqlite_list_child_id(list, i);
    const auto* agg = Node<SyntaqlitePerfettoAggregate>(c->parser(), agg_id);
    ASSIGN_OR_RETURN(ColumnId value, c->ResolveSum(agg_id, agg->expr));
    std::string name = SpanText(c->parser(), agg->name);
    ColumnId id = c->AddColumn(name, core::Int64{});
    acc.aggregates_.push_back({TreeAccumulate::Function::kSum, value, id});
    output.push_back({NamedColumn{std::move(name), id}, agg_id});
  }
  // All expressions see the input scope. Add this stage's columns only after
  // resolving every aggregate; duplicate names are ambiguous on lookup.
  for (auto& [column, node] : output) {
    c->Append(std::move(column), node);
  }
  c->AddNode(std::move(acc), {c->plan().root()});
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipeline
