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
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"

namespace perfetto::trace_processor::pipesql {
namespace {

using pipeline::Builder;
using pipeline::ColumnId;
using pipeline::NamedColumn;
using pipeline::NameId;
using pipeline::TreeDirection;

base::StatusOr<ColumnId> FindRequiredColumn(Translator* t,
                                            const Builder::ScanResult& relation,
                                            uint32_t source,
                                            const char* what,
                                            const char* name) {
  std::optional<ColumnId> column =
      t->FindColumn(relation.columns, t->Name(name));
  if (!column) {
    return t->Expected(source,
                       std::string(what) + " to have a column '" + name + "'");
  }
  return *column;
}

}  // namespace

base::Status TranslateTreeAccumulate(Translator* t, uint32_t stage) {
  t->SetOperation("TREE ACCUMULATE");
  const auto* n = Node<SyntaqlitePerfettoTreeAccumulate>(t->parser(), stage);

  TreeDirection direction =
      n->direction == SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN
          ? TreeDirection::kDown
          : TreeDirection::kUp;
  ASSIGN_OR_RETURN(ColumnId node_column, t->Resolve(t->Name("id"), stage));
  ASSIGN_OR_RETURN(ColumnId parent_column,
                   t->Resolve(t->Name("parent_id"), stage));

  ASSIGN_OR_RETURN(Translator::TranslatedAggregates aggs,
                   t->TranslateAggregateList(n->aggregates));
  auto built = t->builder().TreeAccumulate(t->root(), direction, node_column,
                                           parent_column, aggs.items);
  t->SetRoot(built.relation);
  for (size_t i = 0; i < aggs.items.size(); ++i) {
    t->Append(NamedColumn{aggs.items[i].name, built.columns[i]}, aggs.nodes[i]);
  }
  return base::OkStatus();
}

base::Status TranslateTreeKeyPaths(Translator* t, uint32_t stage) {
  t->SetOperation("TREE KEY PATHS");
  const auto* n = Node<SyntaqlitePerfettoTreeKeyPaths>(t->parser(), stage);

  ASSIGN_OR_RETURN(ColumnId node_column, t->Resolve(t->Name("id"), stage));
  ASSIGN_OR_RETURN(ColumnId parent_column,
                   t->Resolve(t->Name("parent_id"), stage));

  std::vector<ColumnId> keys;
  const auto* list =
      Node<SyntaqlitePerfettoPipeColumnList>(t->parser(), n->columns);
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(t->parser(), item_id);
    std::optional<NameId> qualifier;
    if (IsSpanPresent(item->qualifier)) {
      qualifier = t->Name(item->qualifier);
    }
    ASSIGN_OR_RETURN(ColumnId key,
                     t->Resolve(qualifier, t->Name(item->name), item_id));
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
      keys.push_back(key);
    }
  }

  NameId path_name = t->Name(n->key_name);
  NameId parent_path_name = t->Name(n->parent_key_name);
  auto built = t->builder().TreeKeyPaths(t->root(), node_column, parent_column,
                                         keys, path_name, parent_path_name);
  t->SetRoot(built.relation);
  t->Append(NamedColumn{path_name, built.path}, stage);
  t->Append(NamedColumn{parent_path_name, built.parent_path}, stage);
  return base::OkStatus();
}

base::Status TranslateGraphTree(Translator* t, uint32_t node) {
  using Kind = core::exec::GraphTreeSpec::Kind;
  const auto* n = Node<SyntaqlitePerfettoGraphTree>(t->parser(), node);
  Kind kind = Kind::kBfs;
  switch (n->kind) {
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_BFS:
      t->SetOperation("GRAPH BFS TREE");
      kind = Kind::kBfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DFS:
      t->SetOperation("GRAPH DFS TREE");
      kind = Kind::kDfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DOMINATOR:
      t->SetOperation("GRAPH DOMINATOR TREE");
      kind = Kind::kDominator;
      break;
  }

  ASSIGN_OR_RETURN(Builder::ScanResult nodes,
                   TranslateSourceRelation(t, n->nodes));
  ASSIGN_OR_RETURN(ColumnId node_col,
                   FindRequiredColumn(t, nodes, n->nodes, "NODES", "id"));
  ASSIGN_OR_RETURN(Builder::ScanResult edges,
                   TranslateSourceRelation(t, n->edges));
  ASSIGN_OR_RETURN(ColumnId source_col,
                   FindRequiredColumn(t, edges, n->edges, "EDGES", "src"));
  ASSIGN_OR_RETURN(ColumnId target_col,
                   FindRequiredColumn(t, edges, n->edges, "EDGES", "dst"));
  ASSIGN_OR_RETURN(Builder::ScanResult seeds,
                   TranslateSourceRelation(t, n->seeds));
  ASSIGN_OR_RETURN(ColumnId seed_col,
                   FindRequiredColumn(t, seeds, n->seeds, "FROM", "id"));

  // The tree's parent_id supersedes any input parent_id column.
  NameId parent_name = t->Name("parent_id");
  std::vector<ColumnId> carried;
  for (NamedColumn& column : nodes.columns) {
    if (t->Same(column.name, parent_name)) {
      continue;
    }
    carried.push_back(column.id);
    t->Append(column, n->nodes);
  }
  if (std::optional<NameId> qualifier = SourceQualifier(
          t, *Node<SyntaqlitePerfettoPipeSource>(t->parser(), n->nodes))) {
    t->AddAlias(*qualifier, 0);
  }
  auto built = t->builder().GraphTree(kind, nodes.relation, node_col, carried,
                                      edges.relation, source_col, target_col,
                                      seeds.relation, seed_col);
  t->SetRoot(built.relation);
  t->Append({parent_name, built.parent}, node);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
