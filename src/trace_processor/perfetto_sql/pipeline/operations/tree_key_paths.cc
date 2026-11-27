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

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/tree_key_paths.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

constexpr OperationRegistration TreeKeyPaths::kRegistration{
    12, &MakeOperation<TreeKeyPaths>};

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
                            c->QueryPositions(keys_.span())};
  c->AddOperator(c->arena().New<ex::TreeKeyPaths>(spec));
}

const OperationRegistration& TreeKeyPaths::registration() const {
  return kRegistration;
}

void TreeKeyPaths::Serialize(PlanCodec& c) {
  c.Pod(&node_column_);
  c.Pod(&parent_column_);
  c.Vec(&keys_);
  c.Pod(&path_);
  c.Pod(&parent_path_);
}

}  // namespace perfetto::trace_processor::pipeline
