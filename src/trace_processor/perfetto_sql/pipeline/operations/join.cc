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

#include "src/trace_processor/perfetto_sql/pipeline/operations/join.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/core/util/arena.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {

namespace ex = core::exec;

constexpr OperationRegistration Join::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_JOIN, &BuildPlan,
    OperationRegistration::Encoding{13, &MakeOperation<Join>}};

namespace {

using JoinNode = SyntaqlitePerfettoPipeJoin;

// Adds the right side's columns to the row, under the right side's alias.
// With USING, the right side's copy of the column is only reachable through
// the alias, so the bare name keeps referring to the input's column.
base::StatusOr<SourceRelation> AppendRelation(Compiler* c,
                                              const JoinNode& n,
                                              std::optional<NameId> using_name,
                                              std::vector<ColumnId>* outputs) {
  const auto* source =
      Node<SyntaqlitePerfettoPipeSource>(c->parser(), n.operand);
  std::optional<NameId> alias = Scan::SourceQualifier(c, *source);
  if (alias && c->FindAlias(*alias)) {
    return c->Expected(n.operand, "a different alias for each relation");
  }
  ASSIGN_OR_RETURN(SourceRelation relation, Scan::BuildRelation(c, n.operand));
  // Sized up front: push_back() would inline its growth code here.
  size_t first = c->row().size();
  outputs->resize(relation.columns.size());
  for (size_t i = 0; i < relation.columns.size(); ++i) {
    const NamedColumn& column = relation.columns[i];
    c->Append(column, n.operand,
              using_name && c->Same(column.name, *using_name));
    (*outputs)[i] = column.id;
  }
  if (alias) {
    c->AddAlias(*alias, first);
  }
  return relation;
}

bool HasColumn(const std::vector<NamedColumn>& columns, ColumnId id) {
  for (const NamedColumn& column : columns) {
    if (column.id == id) {
      return true;
    }
  }
  return false;
}

// Finds the probe and build keys from `ON a = b` or `USING (col)`.
base::Status ResolveKeys(Compiler* c,
                         const SourceRelation& relation,
                         uint32_t match,
                         std::optional<NameId> using_name,
                         ColumnId* probe_key,
                         ColumnId* build_key) {
  if (using_name) {
    ASSIGN_OR_RETURN(*probe_key, c->Resolve(*using_name, match));
    std::optional<ColumnId> right =
        c->FindColumn(relation.columns, *using_name);
    if (!right) {
      return c->Expected(match, "a column of each side to be equal");
    }
    *build_key = *right;
    return base::OkStatus();
  }
  const auto* eq = Node<SyntaqliteBinaryExpr>(c->parser(), match);
  if (eq->tag != SYNTAQLITE_NODE_BINARY_EXPR ||
      eq->op != SYNTAQLITE_BINARY_OP_EQ) {
    return c->Unsupported(
        match, "a JOIN condition other than one column equal to another");
  }
  ASSIGN_OR_RETURN(ColumnId a, c->ResolveColumnExpr(eq->left));
  ASSIGN_OR_RETURN(ColumnId b, c->ResolveColumnExpr(eq->right));
  bool a_right = HasColumn(relation.columns, a);
  if (a_right == HasColumn(relation.columns, b)) {
    return c->Expected(match, "a column of each side to be equal");
  }
  *probe_key = a_right ? b : a;
  *build_key = a_right ? a : b;
  return base::OkStatus();
}

}  // namespace

base::Status Join::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("JOIN");
  const auto& n = *Node<JoinNode>(c->parser(), stage);
  bool has_on = syntaqlite_node_is_present(n.on_expr);
  bool has_using = syntaqlite_node_is_present(n.using_columns);
  if (!has_on && !has_using) {
    return c->Expected(stage, "ON or USING to say which rows match");
  }
  Join join;
  join.left_ = n.join_type == SYNTAQLITE_JOIN_TYPE_LEFT;
  if (!join.left_ && n.join_type != SYNTAQLITE_JOIN_TYPE_INNER) {
    return c->Unsupported(stage, "a JOIN other than an INNER or LEFT one");
  }
  uint32_t match = n.on_expr;
  std::optional<NameId> using_name;
  if (!has_on) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeNameList>(c->parser(), n.using_columns);
    if (syntaqlite_list_count(list) != 1) {
      return c->Unsupported(n.using_columns, "a JOIN on more than one column");
    }
    match = syntaqlite_list_child_id(list, 0);
    using_name =
        c->Name(Node<SyntaqlitePerfettoPipeName>(c->parser(), match)->name);
  }
  PlanNodeId input = c->plan().root();
  std::vector<ColumnId> outputs;
  ASSIGN_OR_RETURN(SourceRelation relation,
                   AppendRelation(c, n, using_name, &outputs));
  join.outputs_ = c->arena().CopySpan<ColumnId>(outputs);
  RETURN_IF_ERROR(ResolveKeys(c, relation, match, using_name, &join.probe_key_,
                              &join.build_key_));
  const PlanType& type = c->plan().columns()[join.probe_key_].type;
  if (type.storage() && !(type.Is<core::Uint32>() || type.Is<core::Int32>() ||
                          type.Is<core::Int64>())) {
    return c->Expected(match, "an integer key");
  }
  c->AddNode(join, {input, relation.node});
  return base::OkStatus();
}

std::optional<uint32_t> Join::Prune(std::vector<bool>* needed) {
  outputs_.RemoveIf([&](ColumnId c) { return !(*needed)[c]; });
  (*needed)[probe_key_] = true;
  (*needed)[build_key_] = true;
  return std::nullopt;
}

InputNeeds Join::Needs(uint32_t input) const {
  // The executor reads the probe key in whatever integer type it's stored as.
  InputNeeds needs;
  needs.types_first.push_back(
      {input == 0 ? probe_key_ : build_key_, core::Int64{}, input == 0});
  return needs;
}

OutputProperties Join::Derive(DeriveContext& c) const {
  // A left join outputs a batch's unmatched rows after its matched ones, so
  // the output has no known order.
  OutputProperties out;
  out.columns = c.input(0).columns;
  for (ColumnId column : outputs_) {
    out.columns.push_back(c.ColumnOf(column));
  }
  out.full_batches = false;
  return out;
}

void Join::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  // The right side runs as its own side pipeline, which ends in the join's
  // build.
  Lowering& build = c->AddSidePipeline();
  build.LowerInput(node, 1);
  uint32_t key = build.QueryPosition(build_key_);
  // The build side stores only the columns the join outputs.
  core::ArenaSpan<bool> unread = build.arena().NewSpan<bool>(
      static_cast<uint32_t>(build.QueryRows().columns.size()));
  std::fill(unread.begin(), unread.end(), true);
  core::ArenaSpan<ex::JoinSpec::Output> outputs =
      c->arena().NewSpan<ex::JoinSpec::Output>(
          static_cast<uint32_t>(outputs_.size()));
  for (uint32_t i = 0; i < outputs_.size(); ++i) {
    uint32_t position = build.QueryPosition(outputs_[i]);
    unread[position] = false;
    const PlanType& type = build.QueryRows().columns[position].type;
    outputs[i] = {position,
                  type.storage().value_or(core::StorageType{core::Int64{}})};
  }
  uint32_t build_pipeline =
      build.FinishSide(build.arena().New<ex::JoinBuild>(key, unread));
  ex::JoinSpec spec{
      left_ ? ex::JoinSpec::Kind::kLeft : ex::JoinSpec::Kind::kInner,
      c->QueryPosition(probe_key_), build_pipeline, outputs};
  c->AddOperator(c->arena().New<ex::Join>(spec));
}

void Join::Serialize(PlanCodec& c) {
  c.Pod(&left_);
  c.Pod(&probe_key_);
  c.Pod(&build_key_);
  c.Vec(&outputs_);
}

const OperationRegistration& Join::registration() const {
  return kRegistration;
}

}  // namespace perfetto::trace_processor::pipeline
