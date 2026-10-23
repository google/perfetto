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

#include "src/trace_processor/perfetto_sql/pipeline/operations/tree_from_intervals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/tree_from_intervals.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;

namespace {

struct QualifiedCol {
  bool is_parent = false;
  ColumnId col = 0;
};

base::Status ParseQualifiedColumn(Compiler* c,
                                  uint32_t expr,
                                  QualifiedCol* out) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  if (node->tag != SYNTAQLITE_NODE_COLUMN_REF) {
    return c->Expected(expr, "a column qualified with 'child.' or 'parent.'");
  }
  const SyntaqliteColumnRef& ref = node->column_ref;
  if (IsSpanPresent(ref.schema)) {
    return c->Unsupported(expr, "a schema-qualified column");
  }
  if (!IsSpanPresent(ref.table)) {
    return c->Expected(expr, "a column qualified with 'child.' or 'parent.'");
  }
  std::string table = base::ToLower(SpanText(p, ref.table));
  if (table == "child") {
    out->is_parent = false;
  } else if (table == "parent") {
    out->is_parent = true;
  } else {
    return c->Expected(expr, "qualifier 'child' or 'parent', got '" +
                                 SpanText(p, ref.table) + "'");
  }
  ASSIGN_OR_RETURN(out->col, c->Resolve(c->Name(ref.column), expr));
  return base::OkStatus();
}

base::Status ParseOnCondition(Compiler* c,
                              uint32_t expr,
                              std::vector<ColumnId>* keys,
                              std::vector<ColumnId>* next_keys) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
      node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
    RETURN_IF_ERROR(
        ParseOnCondition(c, node->binary_expr.left, keys, next_keys));
    return ParseOnCondition(c, node->binary_expr.right, keys, next_keys);
  }
  if (node->tag != SYNTAQLITE_NODE_BINARY_EXPR ||
      (node->binary_expr.op != SYNTAQLITE_BINARY_OP_EQ &&
       node->binary_expr.op != SYNTAQLITE_BINARY_OP_EQ_DOUBLE)) {
    return c->Unsupported(
        expr,
        "a condition other than child.<col> = parent.<col> joined by AND");
  }

  QualifiedCol lhs;
  QualifiedCol rhs;
  RETURN_IF_ERROR(ParseQualifiedColumn(c, node->binary_expr.left, &lhs));
  RETURN_IF_ERROR(ParseQualifiedColumn(c, node->binary_expr.right, &rhs));
  if (lhs.is_parent == rhs.is_parent) {
    return c->Expected(
        expr, "one column qualified with 'child.' and one with 'parent.'");
  }

  ColumnId parent_col = lhs.is_parent ? lhs.col : rhs.col;
  ColumnId child_col = lhs.is_parent ? rhs.col : lhs.col;
  if (std::find(keys->begin(), keys->end(), parent_col) != keys->end()) {
    return c->Expected(expr,
                       "each parent column to be matched at most once in ON");
  }
  keys->push_back(parent_col);
  next_keys->push_back(child_col);
  return base::OkStatus();
}

}  // namespace

const OperationRegistration TreeFromIntervals::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_TREE_FROM_INTERVALS, &BuildPlan,
    OperationRegistration::Encoding{9, &MakeOperation<TreeFromIntervals>}};

base::Status TreeFromIntervals::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("TREE FROM INTERVALS");
  const auto* n = Node<SyntaqlitePerfettoTreeFromIntervals>(c->parser(), stage);

  TreeFromIntervals tree;
  NameId ts_name = c->Name("ts");
  NameId dur_name = c->Name("dur");
  NameId id_name = c->Name("id");
  NameId parent_id_name = c->Name("parent_id");
  auto is_bounds = [&](NameId name) {
    return c->Same(name, ts_name) || c->Same(name, dur_name);
  };
  auto is_tree_col = [&](NameId name) {
    return c->Same(name, id_name) || c->Same(name, parent_id_name);
  };
  ASSIGN_OR_RETURN(tree.in_ts_, c->Resolve(ts_name, stage));
  ASSIGN_OR_RETURN(tree.in_dur_, c->Resolve(dur_name, stage));

  RETURN_IF_ERROR(
      ParseOnCondition(c, n->on_expr, &tree.keys_, &tree.next_keys_));

  tree.out_ts_ = c->AddColumn(ts_name, core::Int64{});
  tree.out_dur_ = c->AddColumn(dur_name, core::Int64{});
  tree.out_id_ = c->AddColumn(id_name, core::Id{});
  tree.out_parent_id_ = c->AddColumn(parent_id_name, core::Uint32{});

  std::vector<Compiler::RowColumn> row;
  row.push_back({{ts_name, tree.out_ts_}, c->operation(), stage, false});
  row.push_back({{dur_name, tree.out_dur_}, c->operation(), stage, false});
  row.push_back({{id_name, tree.out_id_}, c->operation(), stage, false});
  row.push_back(
      {{parent_id_name, tree.out_parent_id_}, c->operation(), stage, false});

  std::vector<std::pair<ColumnId, ColumnId>> replaced;
  replaced.emplace_back(tree.in_ts_, tree.out_ts_);
  replaced.emplace_back(tree.in_dur_, tree.out_dur_);
  for (const Compiler::RowColumn& column : c->row()) {
    if (std::find(tree.carried_.begin(), tree.carried_.end(),
                  column.column.id) == tree.carried_.end()) {
      tree.carried_.push_back(column.column.id);
      if (column.column.id != tree.in_ts_ && column.column.id != tree.in_dur_) {
        replaced.emplace_back(column.column.id, column.column.id);
      }
    }
    Compiler::RowColumn out = column;
    if (is_bounds(column.column.name) || is_tree_col(column.column.name)) {
      out.qualified_only = true;
    }
    row.push_back(std::move(out));
  }

  c->ReplaceRow(std::move(row));
  c->ReplaceAliasColumns(replaced);
  c->AddNode(std::move(tree), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> TreeFromIntervals::Prune(std::vector<bool>* needed) {
  auto& tree = *this;
  tree.carried_.erase(
      std::remove_if(tree.carried_.begin(), tree.carried_.end(),
                     [&](ColumnId id) { return !(*needed)[id]; }),
      tree.carried_.end());

  (*needed)[tree.in_ts_] = true;
  (*needed)[tree.in_dur_] = true;
  for (ColumnId key : tree.keys_) {
    (*needed)[key] = true;
  }
  for (ColumnId key : tree.next_keys_) {
    (*needed)[key] = true;
  }
  return std::nullopt;
}

void TreeFromIntervals::Lower(Lowering* c, const PlanNode& node) const {
  const auto& tree = *this;
  c->LowerNode(node.children()[0]);

  for (size_t k = 0; k < tree.keys_.size(); ++k) {
    c->RequireCommonType({{c, tree.keys_[k]}, {c, tree.next_keys_[k]}});
  }

  ex::TreeFromIntervalsSpec spec;
  spec.input = c->RequireIntervalLanes(tree.in_ts_, tree.in_dur_, tree.keys_);
  for (ColumnId next_key : tree.next_keys_) {
    spec.next_key_columns.push_back(c->QueryPosition(next_key));
  }

  spec.outputs.push_back({ex::TreeFromIntervalsSpec::Source::kTs, 0});
  spec.outputs.push_back({ex::TreeFromIntervalsSpec::Source::kDur, 0});
  spec.outputs.push_back({ex::TreeFromIntervalsSpec::Source::kId, 0});
  spec.outputs.push_back({ex::TreeFromIntervalsSpec::Source::kParentId, 0});

  for (ColumnId id : tree.carried_) {
    auto idx = static_cast<uint32_t>(spec.retained_columns.size());
    spec.retained_columns.push_back(c->QueryPosition(id));
    spec.outputs.push_back({ex::TreeFromIntervalsSpec::Source::kInput, idx});
  }

  c->AddOperator(std::make_unique<ex::TreeFromIntervals>(std::move(spec)));

  c->ResetLayout();
  c->DeclareColumn(tree.out_ts_);
  c->DeclareColumn(tree.out_dur_);
  c->DeclareColumn(tree.out_id_);
  c->DeclareColumn(tree.out_parent_id_);
  for (ColumnId id : tree.carried_) {
    c->DeclareColumn(id);
  }
  Lowering::TreeColumns numbers;
  numbers.node = c->DeclareTemporaryColumn();
  numbers.parent = c->DeclareTemporaryColumn();
  c->DeclareTreeWalk(tree.out_id_, tree.out_parent_id_, numbers,
                     TreeDirection::kDown);
}

const OperationRegistration& TreeFromIntervals::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> TreeFromIntervals::Clone() const {
  return std::make_unique<TreeFromIntervals>(*this);
}

void TreeFromIntervals::Serialize(PlanCodec& c) {
  c.Pod(&in_ts_);
  c.Pod(&in_dur_);
  c.Vec(&keys_);
  c.Vec(&next_keys_);
  c.Pod(&out_ts_);
  c.Pod(&out_dur_);
  c.Pod(&out_id_);
  c.Pod(&out_parent_id_);
  c.Vec(&carried_);
}

}  // namespace perfetto::trace_processor::pipeline
