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
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/distinct.h"
#include "src/trace_processor/core/exec/join.h"
#include "src/trace_processor/perfetto_sql/pipeline/builder.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipesql/expressions.h"

namespace perfetto::trace_processor::pipesql {
namespace {

using pipeline::Builder;
using pipeline::ColumnId;
using pipeline::Literal;
using pipeline::NamedColumn;
using pipeline::NameId;
using pipeline::PlanType;
using pipeline::SortKey;

constexpr char kWhereOperators[] =
    "an operator other than =, !=, <, <=, >, >=, GLOB, REGEXP, IN, BETWEEN "
    "and IS [NOT] NULL";

class WhereParser {
 public:
  WhereParser(Translator* t, std::vector<Builder::WhereCondition>* out)
      : t_(t), out_(out) {}

  base::Status Condition(uint32_t expr) {
    const auto* node = Node<SyntaqliteNode>(t_->parser(), expr);
    if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
        node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
      RETURN_IF_ERROR(Condition(node->binary_expr.left));
      return Condition(node->binary_expr.right);
    }
    switch (static_cast<int>(node->tag)) {
      case SYNTAQLITE_NODE_BINARY_EXPR:
        return Comparison(expr, node->binary_expr);
      case SYNTAQLITE_NODE_LIKE_EXPR:
        return Like(expr, node->like_expr);
      case SYNTAQLITE_NODE_IN_EXPR:
        return In(expr, node->in_expr);
      case SYNTAQLITE_NODE_IS_EXPR:
        return Is(expr, node->is_expr);
      case SYNTAQLITE_NODE_BETWEEN_EXPR:
        return Between(expr, node->between_expr);
      default:
        return t_->Unsupported(expr, "a condition other than column OP value");
    }
  }

 private:
  base::Status Comparison(uint32_t expr, const SyntaqliteBinaryExpr& node) {
    std::optional<core::Op> op = ComparisonOp(node.op);
    if (!op) {
      return t_->Unsupported(expr, kWhereOperators);
    }
    ASSIGN_OR_RETURN(ColumnId column, t_->ResolveColumnExpr(node.left));
    return Add(expr, column, *op, {&node.right, &node.right + 1});
  }

  base::Status Like(uint32_t expr, const SyntaqliteLikeExpr& node) {
    core::Op op = core::Glob{};
    switch (static_cast<int>(node.keyword)) {
      case SYNTAQLITE_LIKE_KEYWORD_GLOB:
        break;
      case SYNTAQLITE_LIKE_KEYWORD_REGEXP:
        op = core::Regex{};
        break;
      default:
        return t_->Unsupported(expr, kWhereOperators);
    }
    if (node.negated) {
      return t_->Unsupported(expr,
                             op.Is<core::Glob>() ? "NOT GLOB" : "NOT REGEXP");
    }
    ASSIGN_OR_RETURN(ColumnId column, t_->ResolveColumnExpr(node.operand));
    RETURN_IF_ERROR(Add(expr, column, op, {&node.pattern, &node.pattern + 1}));
    const Literal& pattern = out_->back().values.back();
    if (op.Is<core::Regex>() && pattern.is_string()) {
      // Match SQLite REGEXP semantics: invalid patterns fail early.
      base::StatusOr<base::Regex> regex = base::Regex::Create(pattern.text);
      if (!regex.ok()) {
        return t_->Expected(
            node.pattern, std::string("a regular expression which compiles: ") +
                              regex.status().c_message());
      }
    }
    return base::OkStatus();
  }

  base::Status In(uint32_t expr, const SyntaqliteInExpr& node) {
    if (node.negated) {
      return t_->Unsupported(expr, "NOT IN");
    }
    const auto* source = Node<SyntaqliteNode>(t_->parser(), node.source);
    if (node.bare_source || source->tag != SYNTAQLITE_NODE_EXPR_LIST) {
      return t_->Unsupported(expr, "IN other than a list of literals");
    }
    ASSIGN_OR_RETURN(ColumnId column, t_->ResolveColumnExpr(node.operand));
    const SyntaqliteExprList& list = source->expr_list;
    return Add(expr, column, core::In{},
               {list.children, list.children + list.count});
  }

  base::Status Is(uint32_t expr, const SyntaqliteIsExpr& node) {
    ASSIGN_OR_RETURN(bool null, TranslateIsNull(t_, expr, node));
    ASSIGN_OR_RETURN(ColumnId column, t_->ResolveColumnExpr(node.left));
    core::Op op = null ? core::Op(core::IsNull{}) : core::Op(core::IsNotNull{});
    return Add(expr, column, op, {});
  }

  base::Status Between(uint32_t expr, const SyntaqliteBetweenExpr& node) {
    if (node.negated) {
      return t_->Unsupported(expr, "NOT BETWEEN");
    }
    ASSIGN_OR_RETURN(ColumnId column, t_->ResolveColumnExpr(node.operand));
    RETURN_IF_ERROR(Add(expr, column, core::Ge{}, {&node.low, &node.low + 1}));
    return Add(expr, column, core::Le{}, {&node.high, &node.high + 1});
  }

  base::Status Add(uint32_t at,
                   ColumnId column,
                   core::Op op,
                   base::Span<const uint32_t> exprs) {
    Builder::WhereCondition cond;
    cond.column = column;
    cond.op = op;
    cond.values.reserve(exprs.size());
    for (uint32_t expr : exprs) {
      ASSIGN_OR_RETURN(Literal literal, TranslateLiteral(t_, expr));
      cond.values.push_back(literal);
    }
    if (std::optional<core::StorageType> type =
            t_->ColumnType(column).storage()) {
      if (std::optional<std::string> mismatch =
              pipeline::CheckLiteralTypes(op, cond.values, *type)) {
        return t_->Expected(at, *mismatch);
      }
    }
    out_->push_back(std::move(cond));
    return base::OkStatus();
  }

  static std::optional<core::Op> ComparisonOp(SyntaqliteBinaryOp op) {
    switch (static_cast<int>(op)) {
      case SYNTAQLITE_BINARY_OP_EQ:
      case SYNTAQLITE_BINARY_OP_EQ_DOUBLE:
        return core::Eq{};
      case SYNTAQLITE_BINARY_OP_NE:
      case SYNTAQLITE_BINARY_OP_NE_ANGLE:
        return core::Ne{};
      case SYNTAQLITE_BINARY_OP_LT:
        return core::Lt{};
      case SYNTAQLITE_BINARY_OP_LE:
        return core::Le{};
      case SYNTAQLITE_BINARY_OP_GT:
        return core::Gt{};
      case SYNTAQLITE_BINARY_OP_GE:
        return core::Ge{};
      default:
        return std::nullopt;
    }
  }

  Translator* t_;
  std::vector<Builder::WhereCondition>* out_;
};

// Adds the right side's columns to the row under the right side's alias.
// With USING, the right side's copy of the column is only reachable through
// the alias, so the bare name keeps referring to the input's column.
base::StatusOr<Builder::ScanResult> AppendJoinRelation(
    Translator* t,
    const SyntaqlitePerfettoPipeJoin& n,
    std::optional<NameId> using_name,
    std::vector<ColumnId>* outputs) {
  const auto* source =
      Node<SyntaqlitePerfettoPipeSource>(t->parser(), n.operand);
  std::optional<NameId> alias = SourceQualifier(t, *source);
  if (alias && t->FindAlias(*alias)) {
    return t->Expected(n.operand, "a different alias for each relation");
  }
  ASSIGN_OR_RETURN(Builder::ScanResult relation,
                   TranslateSourceRelation(t, n.operand));
  size_t first = t->row().size();
  outputs->resize(relation.columns.size());
  for (size_t i = 0; i < relation.columns.size(); ++i) {
    const NamedColumn& column = relation.columns[i];
    t->Append(column, n.operand,
              using_name && t->Same(column.name, *using_name));
    (*outputs)[i] = column.id;
  }
  if (alias) {
    t->AddAlias(*alias, first);
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

base::Status ResolveJoinKeys(Translator* t,
                             const Builder::ScanResult& relation,
                             uint32_t match,
                             std::optional<NameId> using_name,
                             ColumnId* probe_key,
                             ColumnId* build_key) {
  if (using_name) {
    ASSIGN_OR_RETURN(*probe_key, t->Resolve(*using_name, match));
    std::optional<ColumnId> right =
        t->FindColumn(relation.columns, *using_name);
    if (!right) {
      return t->Expected(match, "a column of each side to be equal");
    }
    *build_key = *right;
    return base::OkStatus();
  }
  const auto* eq = Node<SyntaqliteBinaryExpr>(t->parser(), match);
  if (eq->tag != SYNTAQLITE_NODE_BINARY_EXPR ||
      eq->op != SYNTAQLITE_BINARY_OP_EQ) {
    return t->Unsupported(
        match, "a JOIN condition other than one column equal to another");
  }
  ASSIGN_OR_RETURN(ColumnId a, t->ResolveColumnExpr(eq->left));
  ASSIGN_OR_RETURN(ColumnId b, t->ResolveColumnExpr(eq->right));
  bool a_right = HasColumn(relation.columns, a);
  if (a_right == HasColumn(relation.columns, b)) {
    return t->Expected(match, "a column of each side to be equal");
  }
  *probe_key = a_right ? b : a;
  *build_key = a_right ? a : b;
  return base::OkStatus();
}

}  // namespace

base::Status TranslateWhere(Translator* t, uint32_t stage) {
  t->SetOperation("WHERE");
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(t->parser(), stage);
  std::vector<Builder::WhereCondition> conditions;
  WhereParser parser(t, &conditions);
  RETURN_IF_ERROR(parser.Condition(n->condition));
  t->SetRoot(t->builder().Where(t->root(), std::move(conditions)));
  return base::OkStatus();
}

base::Status TranslateAggregate(Translator* t, uint32_t stage) {
  t->SetOperation("AGGREGATE");
  const auto* n = Node<SyntaqlitePerfettoPipeAggregate>(t->parser(), stage);

  std::vector<ColumnId> group_keys;
  std::vector<std::pair<NamedColumn, uint32_t>> keys;
  if (syntaqlite_node_is_present(n->group_by)) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeColumnList>(t->parser(), n->group_by);
    for (uint32_t i = 0; i < syntaqlite_list_count(list); ++i) {
      uint32_t item_id = syntaqlite_list_child_id(list, i);
      const auto* item =
          Node<SyntaqlitePerfettoPipeColumn>(t->parser(), item_id);
      std::optional<NameId> qualifier;
      if (IsSpanPresent(item->qualifier)) {
        qualifier = t->Name(item->qualifier);
      }
      NameId name = t->Name(item->name);
      ASSIGN_OR_RETURN(ColumnId key, t->Resolve(qualifier, name, item_id));
      if (IsSpanPresent(item->alias)) {
        name = t->Name(item->alias);
      }
      // Groups once by each column, but outputs one column per item listed.
      if (std::find(group_keys.begin(), group_keys.end(), key) ==
          group_keys.end()) {
        group_keys.push_back(key);
      }
      keys.push_back({NamedColumn{name, key}, item_id});
    }
  }

  ASSIGN_OR_RETURN(Translator::TranslatedAggregates aggs,
                   t->TranslateAggregateList(n->aggregates));
  auto built = t->builder().Aggregate(t->root(), group_keys, aggs.items);
  t->SetRoot(built.relation);

  t->ReplaceRow({});
  t->ClearAliases();
  for (auto& [column, node] : keys) {
    t->Append(column, node);
  }
  for (size_t i = 0; i < aggs.items.size(); ++i) {
    t->Append(NamedColumn{aggs.items[i].name, built.columns[i]}, aggs.nodes[i]);
  }
  return base::OkStatus();
}

base::Status TranslateDistinct(Translator* t, uint32_t stage) {
  t->SetOperation("DISTINCT");
  std::vector<ColumnId> keys;
  std::vector<Translator::RowColumn> row;
  std::vector<std::pair<ColumnId, ColumnId>> kept;
  for (const Translator::RowColumn& column : t->row()) {
    if (!column.qualified_only) {
      keys.push_back(column.column.id);
      row.push_back(column);
      kept.emplace_back(column.column.id, column.column.id);
    }
  }
  if (keys.size() > core::exec::Distinct::kMaxColumns) {
    return t->Unsupported(stage, "a row of more than 64 columns");
  }
  t->ReplaceRow(std::move(row));
  t->ReplaceAliasColumns(kept);
  t->SetRoot(t->builder().Distinct(t->root(), keys));
  return base::OkStatus();
}

base::Status TranslateJoin(Translator* t, uint32_t stage) {
  t->SetOperation("JOIN");
  const auto& n = *Node<SyntaqlitePerfettoPipeJoin>(t->parser(), stage);
  bool has_on = syntaqlite_node_is_present(n.on_expr);
  bool has_using = syntaqlite_node_is_present(n.using_columns);
  if (!has_on && !has_using) {
    return t->Expected(stage, "ON or USING to say which rows match");
  }
  bool left = n.join_type == SYNTAQLITE_JOIN_TYPE_LEFT;
  if (!left && n.join_type != SYNTAQLITE_JOIN_TYPE_INNER) {
    return t->Unsupported(stage, "a JOIN other than an INNER or LEFT one");
  }
  uint32_t match = n.on_expr;
  std::optional<NameId> using_name;
  if (!has_on) {
    const auto* list =
        Node<SyntaqlitePerfettoPipeNameList>(t->parser(), n.using_columns);
    if (syntaqlite_list_count(list) != 1) {
      return t->Unsupported(n.using_columns, "a JOIN on more than one column");
    }
    match = syntaqlite_list_child_id(list, 0);
    using_name =
        t->Name(Node<SyntaqlitePerfettoPipeName>(t->parser(), match)->name);
  }
  Builder::Relation input = t->root();
  std::vector<ColumnId> outputs;
  ASSIGN_OR_RETURN(Builder::ScanResult relation,
                   AppendJoinRelation(t, n, using_name, &outputs));
  ColumnId probe_key = 0;
  ColumnId build_key = 0;
  RETURN_IF_ERROR(
      ResolveJoinKeys(t, relation, match, using_name, &probe_key, &build_key));
  PlanType type = t->ColumnType(probe_key);
  if (type.storage() && !(type.Is<core::Uint32>() || type.Is<core::Int32>() ||
                          type.Is<core::Int64>())) {
    return t->Expected(match, "an integer key");
  }
  t->SetRoot(t->builder().Join(input, relation.relation,
                               left ? core::exec::JoinSpec::Kind::kLeft
                                    : core::exec::JoinSpec::Kind::kInner,
                               probe_key, build_key, outputs));
  return base::OkStatus();
}

base::Status TranslateOrderBy(Translator* t, uint32_t stage) {
  t->SetOperation("ORDER BY");
  const auto* n = Node<SyntaqlitePerfettoPipeOrderBy>(t->parser(), stage);
  const auto* terms =
      Node<SyntaqlitePerfettoPipeOrderTermList>(t->parser(), n->terms);

  std::vector<SortKey> keys;
  for (uint32_t i = 0; i < syntaqlite_list_count(terms); ++i) {
    uint32_t term_id = syntaqlite_list_child_id(terms, i);
    const auto* term =
        Node<SyntaqlitePerfettoPipeOrderTerm>(t->parser(), term_id);
    ASSIGN_OR_RETURN(ColumnId id, t->ResolveColumnExpr(term->expr));
    keys.push_back({id, term->sort_order == SYNTAQLITE_SORT_ORDER_DESC});
  }
  PERFETTO_DCHECK(!keys.empty());
  t->SetRoot(t->builder().OrderBy(t->root(), std::move(keys)));
  return base::OkStatus();
}

base::Status TranslateLimit(Translator* t, uint32_t stage) {
  t->SetOperation("LIMIT");
  const auto* n = Node<SyntaqlitePerfettoPipeLimit>(t->parser(), stage);
  ASSIGN_OR_RETURN(int64_t count, TranslateInt64Literal(t, n->count));
  int64_t offset = 0;
  if (syntaqlite_node_is_present(n->offset)) {
    ASSIGN_OR_RETURN(offset, TranslateInt64Literal(t, n->offset));
  }
  t->SetRoot(t->builder().Limit(t->root(), count, offset));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
