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
#include "src/trace_processor/core/exec/graph_tree.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

using Kind = ex::GraphTreeSpec::Kind;

const OperationRegistration GraphTree::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_GRAPH_TREE, &BuildPlan,
    OperationRegistration::Encoding{9, &MakeOperation<GraphTree>}};

namespace {

// Finds the column `name` of the relation read from `source`.
base::StatusOr<ColumnId> FindColumn(Compiler* c,
                                    const SourceRelation& relation,
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
  GraphTree tree;
  switch (n->kind) {
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_BFS:
      c->SetOperation("GRAPH BFS TREE");
      tree.kind_ = Kind::kBfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DFS:
      c->SetOperation("GRAPH DFS TREE");
      tree.kind_ = Kind::kDfs;
      break;
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_DOMINATOR:
      c->SetOperation("GRAPH DOMINATOR TREE");
      tree.kind_ = Kind::kDominator;
      break;
  }

  ASSIGN_OR_RETURN(SourceRelation nodes, Scan::BuildRelation(c, n->nodes));
  ASSIGN_OR_RETURN(tree.node_, FindColumn(c, nodes, n->nodes, "NODES", "id"));
  ASSIGN_OR_RETURN(SourceRelation edges, Scan::BuildRelation(c, n->edges));
  ASSIGN_OR_RETURN(tree.source_,
                   FindColumn(c, edges, n->edges, "EDGES", "src"));
  ASSIGN_OR_RETURN(tree.target_,
                   FindColumn(c, edges, n->edges, "EDGES", "dst"));
  ASSIGN_OR_RETURN(SourceRelation seeds, Scan::BuildRelation(c, n->seeds));
  ASSIGN_OR_RETURN(tree.seed_, FindColumn(c, seeds, n->seeds, "FROM", "id"));

  // The tree's parent_id is what a parent_id means after this, so a node
  // column of that name is not carried.
  NameId parent_name = c->Name("parent_id");
  for (NamedColumn& column : nodes.columns) {
    if (c->Same(column.name, parent_name)) {
      continue;
    }
    tree.carried_.push_back(column.id);
    c->Append(std::move(column), n->nodes);
  }
  if (std::optional<NameId> qualifier = Scan::SourceQualifier(
          c, *Node<SyntaqlitePerfettoPipeSource>(c->parser(), n->nodes))) {
    c->AddAlias({*qualifier, c->row()});
  }
  tree.parent_ = c->AddColumn(parent_name, core::Int64{});
  c->Append({parent_name, tree.parent_}, node);
  c->AddNode(std::move(tree), {nodes.node, edges.node, seeds.node});
  return base::OkStatus();
}

std::optional<uint32_t> GraphTree::Prune(std::vector<bool>* needed) {
  carried_.erase(std::remove_if(carried_.begin(), carried_.end(),
                                [&](ColumnId id) { return !(*needed)[id]; }),
                 carried_.end());
  (*needed)[node_] = true;
  (*needed)[source_] = true;
  (*needed)[target_] = true;
  (*needed)[seed_] = true;
  return std::nullopt;
}

void GraphTree::Lower(Lowering* c, const PlanNode& node) const {
  const auto& children = node.children();
  PERFETTO_DCHECK(!c->has_source());
  PERFETTO_DCHECK(children.size() == 3);

  // Ids are compared across the inputs as integers. Nothing may reorder the
  // seeds or edges: their order picks between equally good parents.
  Lowering& nodes = c->AddSidePipeline();
  nodes.LowerNode(children[0]);
  nodes.RequireType(node_, core::Int64{});
  Lowering& edges = c->AddSidePipeline();
  edges.LowerNode(children[1]);
  edges.RequireType(source_, core::Int64{});
  edges.RequireType(target_, core::Int64{});
  Lowering& seeds = c->AddSidePipeline();
  seeds.LowerNode(children[2]);
  seeds.RequireType(seed_, core::Int64{});

  ex::GraphTreeSpec spec;
  spec.kind = kind_;
  spec.node_column = nodes.QueryPosition(node_);
  for (ColumnId id : carried_) {
    spec.retained_columns.push_back(nodes.QueryPosition(id));
  }
  spec.nodes = nodes.FinishSide();
  spec.source_column = edges.QueryPosition(source_);
  spec.target_column = edges.QueryPosition(target_);
  spec.edges = edges.FinishSide();
  spec.seed_column = seeds.QueryPosition(seed_);
  spec.seeds = seeds.FinishSide();

  for (ColumnId id : carried_) {
    c->DeclareColumn(id);
  }
  c->DeclareColumn(parent_);
  Lowering::TreeColumns numbers;
  numbers.node = c->DeclareTemporaryColumn();
  numbers.parent = c->DeclareTemporaryColumn();
  c->SetSource(std::make_unique<ex::GraphTree>(std::move(spec)));
  c->DeclareTreeWalk(node_, parent_, numbers, TreeDirection::kDown);
}

const OperationRegistration& GraphTree::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> GraphTree::Clone() const {
  return std::make_unique<GraphTree>(*this);
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
