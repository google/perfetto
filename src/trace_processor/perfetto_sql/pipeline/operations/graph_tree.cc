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
    case SYNTAQLITE_PERFETTO_GRAPH_TREE_KIND_CRITICAL_PATH:
      c->SetOperation("GRAPH CRITICAL PATH");
      tree.kind_ = Kind::kCriticalPath;
      break;
  }

  ASSIGN_OR_RETURN(SourceRelation nodes, Scan::BuildRelation(c, n->nodes));
  ASSIGN_OR_RETURN(tree.node_, FindColumn(c, nodes, n->nodes, "NODES", "id"));
  if (tree.kind_ == Kind::kCriticalPath) {
    ASSIGN_OR_RETURN(tree.node_ts_,
                     FindColumn(c, nodes, n->nodes, "NODES", "ts"));
    ASSIGN_OR_RETURN(tree.node_dur_,
                     FindColumn(c, nodes, n->nodes, "NODES", "dur"));
    tree.node_lane_ = tree.node_;
    if (syntaqlite_node_is_present(n->per)) {
      const auto* per =
          Node<SyntaqlitePerfettoPerColumnList>(c->parser(), n->per);
      if (syntaqlite_list_count(per) > 1) {
        return c->Expected(n->per, "at most one PER column");
      }
      if (syntaqlite_list_count(per) == 1) {
        uint32_t col_id = syntaqlite_list_child_id(per, 0);
        NameId lane_name = c->Name(
            Node<SyntaqlitePerfettoPerColumn>(c->parser(), col_id)->name);
        std::optional<ColumnId> lane_col =
            c->FindColumn(nodes.columns, lane_name);
        if (!lane_col) {
          return c->Expected(
              col_id, "NODES to have a column '" + c->Text(lane_name) + "'");
        }
        tree.node_lane_ = *lane_col;
      }
    }
  }
  ASSIGN_OR_RETURN(SourceRelation edges, Scan::BuildRelation(c, n->edges));
  ASSIGN_OR_RETURN(tree.source_,
                   FindColumn(c, edges, n->edges, "EDGES", "src"));
  ASSIGN_OR_RETURN(tree.target_,
                   FindColumn(c, edges, n->edges, "EDGES", "dst"));
  if (tree.kind_ == Kind::kCriticalPath) {
    ASSIGN_OR_RETURN(tree.edge_ts_,
                     FindColumn(c, edges, n->edges, "EDGES", "ts"));
    ASSIGN_OR_RETURN(tree.edge_dur_,
                     FindColumn(c, edges, n->edges, "EDGES", "dur"));
  }
  ASSIGN_OR_RETURN(SourceRelation seeds, Scan::BuildRelation(c, n->seeds));
  ASSIGN_OR_RETURN(tree.seed_, FindColumn(c, seeds, n->seeds, "FROM", "id"));
  if (tree.kind_ == Kind::kCriticalPath) {
    std::optional<ColumnId> s_ts = c->FindColumn(seeds.columns, c->Name("ts"));
    std::optional<ColumnId> s_dur =
        c->FindColumn(seeds.columns, c->Name("dur"));
    if (s_ts.has_value() != s_dur.has_value()) {
      return c->Expected(n->seeds,
                         "FROM to have both 'ts' and 'dur' columns or neither");
    }
    tree.seed_ts_ = s_ts;
    tree.seed_dur_ = s_dur;
  }

  // The tree's parent_id is what a parent_id means after this, so a node
  // column of that name is not carried.
  NameId parent_name = c->Name("parent_id");
  if (tree.kind_ == Kind::kCriticalPath) {
    NameId ts_name = c->Name("ts");
    NameId dur_name = c->Name("dur");
    NameId root_id_name = c->Name("root_id");
    NameId depth_name = c->Name("depth");
    tree.out_ts_ = c->AddColumn(ts_name, core::Int64{});
    tree.out_dur_ = c->AddColumn(dur_name, core::Int64{});
    tree.root_id_ = c->AddColumn(root_id_name, core::Int64{});
    tree.depth_ = c->AddColumn(depth_name, core::Int64{});
    for (NamedColumn& column : nodes.columns) {
      if (c->Same(column.name, parent_name) ||
          c->Same(column.name, root_id_name) ||
          c->Same(column.name, depth_name)) {
        continue;
      }
      if (column.id == tree.node_) {
        c->Append(std::move(column), n->nodes);
      } else if (column.id == tree.node_ts_) {
        c->Append({ts_name, tree.out_ts_}, node);
      } else if (column.id == tree.node_dur_) {
        c->Append({dur_name, tree.out_dur_}, node);
      } else {
        tree.carried_.push_back(column.id);
        c->Append(std::move(column), n->nodes);
      }
    }
    if (std::optional<NameId> qualifier = Scan::SourceQualifier(
            c, *Node<SyntaqlitePerfettoPipeSource>(c->parser(), n->nodes))) {
      c->AddAlias({*qualifier, c->row()});
    }
    for (NamedColumn& column : seeds.columns) {
      if (column.id == tree.seed_ || column.id == tree.seed_ts_ ||
          column.id == tree.seed_dur_ || c->Same(column.name, parent_name) ||
          c->Same(column.name, root_id_name) ||
          c->Same(column.name, depth_name) ||
          c->FindColumn(nodes.columns, column.name).has_value()) {
        continue;
      }
      tree.seed_carried_.push_back(column.id);
      c->Append(std::move(column), n->seeds);
    }
    c->Append({root_id_name, tree.root_id_}, node);
    c->Append({depth_name, tree.depth_}, node);
    tree.parent_ = c->AddColumn(parent_name, core::Int64{});
    c->Append({parent_name, tree.parent_}, node);
    c->AddNode(std::move(tree), {nodes.node, edges.node, seeds.node});
    return base::OkStatus();
  }

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
  seed_carried_.erase(
      std::remove_if(seed_carried_.begin(), seed_carried_.end(),
                     [&](ColumnId id) { return !(*needed)[id]; }),
      seed_carried_.end());
  (*needed)[node_] = true;
  (*needed)[source_] = true;
  (*needed)[target_] = true;
  (*needed)[seed_] = true;
  if (kind_ == Kind::kCriticalPath) {
    (*needed)[node_ts_] = true;
    (*needed)[node_dur_] = true;
    (*needed)[node_lane_] = true;
    (*needed)[edge_ts_] = true;
    (*needed)[edge_dur_] = true;
    if (seed_ts_) {
      (*needed)[*seed_ts_] = true;
    }
    if (seed_dur_) {
      (*needed)[*seed_dur_] = true;
    }
  }
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
  if (kind_ == Kind::kCriticalPath) {
    nodes.RequireType(node_ts_, core::Int64{});
    nodes.RequireType(node_dur_, core::Int64{});
    nodes.RequireType(node_lane_, core::Int64{});
  }
  Lowering& edges = c->AddSidePipeline();
  edges.LowerNode(children[1]);
  edges.RequireType(source_, core::Int64{});
  edges.RequireType(target_, core::Int64{});
  if (kind_ == Kind::kCriticalPath) {
    edges.RequireType(edge_ts_, core::Int64{});
    edges.RequireType(edge_dur_, core::Int64{});
  }
  Lowering& seeds = c->AddSidePipeline();
  seeds.LowerNode(children[2]);
  seeds.RequireType(seed_, core::Int64{});
  if (kind_ == Kind::kCriticalPath) {
    if (seed_ts_) {
      seeds.RequireType(*seed_ts_, core::Int64{});
    }
    if (seed_dur_) {
      seeds.RequireType(*seed_dur_, core::Int64{});
    }
  }

  ex::GraphTreeSpec spec;
  spec.kind = kind_;
  spec.node_column = nodes.QueryPosition(node_);
  if (kind_ == Kind::kCriticalPath) {
    spec.node_ts_column = nodes.QueryPosition(node_ts_);
    spec.node_dur_column = nodes.QueryPosition(node_dur_);
    spec.node_lane_column = nodes.QueryPosition(node_lane_);
  }
  for (ColumnId id : carried_) {
    spec.retained_columns.push_back(nodes.QueryPosition(id));
  }
  spec.nodes = nodes.FinishSide();
  spec.source_column = edges.QueryPosition(source_);
  spec.target_column = edges.QueryPosition(target_);
  if (kind_ == Kind::kCriticalPath) {
    spec.edge_ts_column = edges.QueryPosition(edge_ts_);
    spec.edge_dur_column = edges.QueryPosition(edge_dur_);
  }
  spec.edges = edges.FinishSide();
  spec.seed_column = seeds.QueryPosition(seed_);
  if (kind_ == Kind::kCriticalPath) {
    if (seed_ts_) {
      spec.seed_ts_column = seeds.QueryPosition(*seed_ts_);
    }
    if (seed_dur_) {
      spec.seed_dur_column = seeds.QueryPosition(*seed_dur_);
    }
    for (ColumnId id : seed_carried_) {
      spec.seed_retained_columns.push_back(seeds.QueryPosition(id));
    }
  }
  spec.seeds = seeds.FinishSide();

  for (ColumnId id : carried_) {
    c->DeclareColumn(id);
  }
  for (ColumnId id : seed_carried_) {
    c->DeclareColumn(id);
  }
  if (kind_ == Kind::kCriticalPath) {
    c->DeclareColumn(node_);
    c->DeclareColumn(out_ts_);
    c->DeclareColumn(out_dur_);
    c->DeclareColumn(root_id_);
    c->DeclareColumn(depth_);
    c->DeclareColumn(parent_);
    uint32_t groups = c->DeclareTemporaryColumn();
    c->SetSource(std::make_unique<ex::GraphTree>(std::move(spec)));
    c->DeclareSortedWithinGroups({root_id_}, {{out_ts_, false}});
    c->DeclareGroupNumbers({root_id_}, groups);
    if (!seed_carried_.empty()) {
      std::vector<ColumnId> all_root_keys = {root_id_};
      all_root_keys.insert(all_root_keys.end(), seed_carried_.begin(),
                           seed_carried_.end());
      c->DeclareSortedWithinGroups(all_root_keys, {{out_ts_, false}});
      c->DeclareGroupNumbers(all_root_keys, groups);
    }
    return;
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
  if (kind_ == Kind::kCriticalPath) {
    c.Pod(&node_ts_);
    c.Pod(&node_dur_);
    c.Pod(&node_lane_);
    c.Pod(&edge_ts_);
    c.Pod(&edge_dur_);
    bool has_seed_bounds = seed_ts_.has_value();
    c.Pod(&has_seed_bounds);
    if (has_seed_bounds) {
      ColumnId ts = seed_ts_.value_or(0);
      ColumnId dur = seed_dur_.value_or(0);
      c.Pod(&ts);
      c.Pod(&dur);
      seed_ts_ = ts;
      seed_dur_ = dur;
    } else {
      seed_ts_.reset();
      seed_dur_.reset();
    }
    c.Vec(&seed_carried_);
    c.Pod(&out_ts_);
    c.Pod(&out_dur_);
    c.Pod(&root_id_);
    c.Pod(&depth_);
  }
}

}  // namespace perfetto::trace_processor::pipeline
