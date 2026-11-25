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

#include "src/trace_processor/perfetto_sql/pipeline/operations/graph_tree.h"

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
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

using Kind = ex::GraphTreeSpec::Kind;

constexpr OperationRegistration GraphTree::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_GRAPH_TREE, &BuildPlan,
    OperationRegistration::Encoding{9, &MakeOperation<GraphTree>}};

namespace {

// Finds the column `name` of the relation read from `source`.
base::StatusOr<ColumnId> FindColumn(Compiler* c,
                                    const Builder::ScanResult& relation,
                                    uint32_t source,
                                    const char* what,
                                    const char* name) {
  std::optional<ColumnId> column =
      c->FindColumn(relation.columns, c->Name(name));
  if (!column) {
    return c->Expected(source,
                       std::string(what) + " to have a column '" + name + "'");
  }
  return *column;
}

}  // namespace

base::Status GraphTree::BuildPlan(Compiler* c, uint32_t node) {
  const auto* n = Node<SyntaqlitePerfettoGraphTree>(c->parser(), node);
  Kind kind = Kind::kBfs;
  switch (n->kind) {
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_BFS:
      c->SetOperation("GRAPH BFS TREE");
      kind = Kind::kBfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DFS:
      c->SetOperation("GRAPH DFS TREE");
      kind = Kind::kDfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DOMINATOR:
      c->SetOperation("GRAPH DOMINATOR TREE");
      kind = Kind::kDominator;
      break;
  }

  ASSIGN_OR_RETURN(Builder::ScanResult nodes, Scan::BuildRelation(c, n->nodes));
  ASSIGN_OR_RETURN(ColumnId node_col,
                   FindColumn(c, nodes, n->nodes, "NODES", "id"));
  ASSIGN_OR_RETURN(Builder::ScanResult edges, Scan::BuildRelation(c, n->edges));
  ASSIGN_OR_RETURN(ColumnId source_col,
                   FindColumn(c, edges, n->edges, "EDGES", "src"));
  ASSIGN_OR_RETURN(ColumnId target_col,
                   FindColumn(c, edges, n->edges, "EDGES", "dst"));
  ASSIGN_OR_RETURN(Builder::ScanResult seeds, Scan::BuildRelation(c, n->seeds));
  ASSIGN_OR_RETURN(ColumnId seed_col,
                   FindColumn(c, seeds, n->seeds, "FROM", "id"));

  // The tree's parent_id is what a parent_id means after this, so a node
  // column of that name is not carried.
  NameId parent_name = c->Name("parent_id");
  std::vector<ColumnId> carried;
  for (NamedColumn& column : nodes.columns) {
    if (c->Same(column.name, parent_name)) {
      continue;
    }
    carried.push_back(column.id);
    c->Append(std::move(column), n->nodes);
  }
  if (std::optional<NameId> qualifier = Scan::SourceQualifier(
          c, *Node<SyntaqlitePerfettoPipeSource>(c->parser(), n->nodes))) {
    c->AddAlias(*qualifier, 0);
  }
  auto built = c->builder().GraphTree(kind, nodes.relation, node_col, carried,
                                      edges.relation, source_col, target_col,
                                      seeds.relation, seed_col);
  c->SetRoot(built.relation);
  c->Append({parent_name, built.parent}, node);
  return base::OkStatus();
}

std::optional<uint32_t> GraphTree::Prune(std::vector<bool>* needed) {
  carried_.RemoveIf([&](ColumnId id) { return !(*needed)[id]; });
  (*needed)[node_] = true;
  (*needed)[source_] = true;
  (*needed)[target_] = true;
  (*needed)[seed_] = true;
  return std::nullopt;
}

InputNeeds GraphTree::Needs(uint32_t input) const {
  // Ids are compared across the inputs as integers.
  InputNeeds needs;
  if (input == 0) {
    needs.types_first.push_back({node_, core::Int64{}, false});
  } else if (input == 1) {
    needs.types_first.push_back({source_, core::Int64{}, false});
    needs.types_first.push_back({target_, core::Int64{}, false});
  } else {
    needs.types_first.push_back({seed_, core::Int64{}, false});
  }
  return needs;
}

OutputProperties GraphTree::Derive(DeriveContext& c) const {
  OutputProperties out;
  for (ColumnId id : carried_) {
    out.columns.push_back(c.ColumnOf(id));
  }
  out.columns.push_back(c.ColumnOf(parent_));
  OutputProperties::Column node_number = c.NewColumn();
  OutputProperties::Column parent_number = c.NewColumn();
  out.columns.push_back(node_number);
  out.columns.push_back(parent_number);
  out.tree = OutputProperties::Tree{node_, parent_, TreeDirection::kDown,
                                    node_number.id, parent_number.id};
  return out;
}

void GraphTree::Lower(Lowering* c, const PlanNode& node) const {
  const auto& children = node.children();
  PERFETTO_DCHECK(!c->has_source());
  PERFETTO_DCHECK(children.size() == 3);

  // Nothing may reorder the seeds or edges: their order picks between equally
  // good parents.
  Lowering& nodes = c->AddSidePipeline();
  nodes.LowerInput(node, 0);
  Lowering& edges = c->AddSidePipeline();
  edges.LowerInput(node, 1);
  Lowering& seeds = c->AddSidePipeline();
  seeds.LowerInput(node, 2);

  ex::GraphTreeSpec spec;
  spec.kind = kind_;
  spec.node_column = nodes.QueryPosition(node_);
  spec.retained_columns = nodes.QueryPositions(carried_);
  spec.nodes = nodes.FinishSide();
  spec.source_column = edges.QueryPosition(source_);
  spec.target_column = edges.QueryPosition(target_);
  spec.edges = edges.FinishSide();
  spec.seed_column = seeds.QueryPosition(seed_);
  spec.seeds = seeds.FinishSide();

  c->SetSource(c->arena().New<ex::GraphTree>(spec));
}

const OperationRegistration& GraphTree::registration() const {
  return kRegistration;
}

void GraphTree::Serialize(PlanCodec& c) {
  c.Pod(&kind_);
  c.Pod(&node_);
  c.Vec(&carried_);
  c.Pod(&source_);
  c.Pod(&target_);
  c.Pod(&seed_);
  c.Pod(&parent_);
}

}  // namespace perfetto::trace_processor::pipeline
