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

#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_key_paths.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/tree_key_paths.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

const OperationRegistration TreeKeyPaths::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_TREE_KEY_PATHS, &BuildPlan,
    OperationRegistration::Encoding{12, &MakeOperation<TreeKeyPaths>}};

base::Status TreeKeyPaths::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("TREE KEY PATHS");
  const auto* n = Node<SyntaqlitePerfettoTreeKeyPaths>(c->parser(), stage);

  TreeKeyPaths paths;
  ASSIGN_OR_RETURN(paths.node_column_, c->Resolve(c->Name("id"), stage));
  ASSIGN_OR_RETURN(paths.parent_column_,
                   c->Resolve(c->Name("parent_id"), stage));

  const auto* list =
      Node<SyntaqlitePerfettoPipeColumnList>(c->parser(), n->columns);
  for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
    uint32_t item_id = syntaqlite_list_child_id(list, i);
    const auto* item = Node<SyntaqlitePerfettoPipeColumn>(c->parser(), item_id);
    std::optional<NameId> qualifier;
    if (IsSpanPresent(item->qualifier)) {
      qualifier = c->Name(item->qualifier);
    }
    ASSIGN_OR_RETURN(ColumnId key,
                     c->Resolve(qualifier, c->Name(item->name), item_id));
    if (std::find(paths.keys_.begin(), paths.keys_.end(), key) ==
        paths.keys_.end()) {
      paths.keys_.push_back(key);
    }
  }

  NameId path_name = c->Name(n->key_name);
  NameId parent_path_name = c->Name(n->parent_key_name);
  paths.path_ = c->AddColumn(path_name, core::Int64{});
  paths.parent_path_ = c->AddColumn(parent_path_name, core::Int64{});
  c->Append(NamedColumn{path_name, paths.path_}, stage);
  c->Append(NamedColumn{parent_path_name, paths.parent_path_}, stage);
  c->AddNode(std::move(paths), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> TreeKeyPaths::Prune(std::vector<bool>* needed) {
  // As with TREE ACCUMULATE, unread outputs skip the stage and its checks.
  if (!(*needed)[path_] && !(*needed)[parent_path_]) {
    return 0;
  }
  (*needed)[node_column_] = true;
  (*needed)[parent_column_] = true;
  for (ColumnId key : keys_) {
    (*needed)[key] = true;
  }
  return std::nullopt;
}

InputNeeds TreeKeyPaths::Needs(uint32_t) const {
  InputNeeds needs;
  needs.tree_walk =
      InputNeeds::TreeWalk{node_column_, parent_column_, TreeDirection::kDown};
  return needs;
}

OutputProperties TreeKeyPaths::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  out.open_sort.reset();
  out.columns.push_back(c.ColumnOf(path_));
  out.columns.push_back(c.ColumnOf(parent_path_));
  return out;
}

void TreeKeyPaths::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  const OutputProperties::Tree& tree = *c->QueryRows().tree;
  ex::TreeKeyPathsSpec spec{c->QueryPosition(tree.node_number),
                            c->QueryPosition(tree.parent_number),
                            {}};
  for (ColumnId key : keys_) {
    spec.key_columns.push_back(c->QueryPosition(key));
  }
  c->AddOperator(std::make_unique<ex::TreeKeyPaths>(std::move(spec)));
}

const OperationRegistration& TreeKeyPaths::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> TreeKeyPaths::Clone() const {
  return std::make_unique<TreeKeyPaths>(*this);
}

void TreeKeyPaths::Serialize(PlanCodec& c) {
  c.Pod(&node_column_);
  c.Pod(&parent_column_);
  c.Vec(&keys_);
  c.Pod(&path_);
  c.Pod(&parent_path_);
}

}  // namespace perfetto::trace_processor::pipeline
