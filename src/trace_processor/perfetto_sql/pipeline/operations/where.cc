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

#include "src/trace_processor/perfetto_sql/pipeline/operations/where.h"

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
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operations/scan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

static constexpr char kOperators[] =
    "an operator other than =, !=, <, <=, >, >=, GLOB, IN, BETWEEN and "
    "IS [NOT] NULL";

// What `p` should compare with to suit a column of storage `type`, if a literal
// of it is a string where the column holds numbers, or the other way around.
// GLOB and IS [NOT] NULL do not compare values.
std::optional<std::string> LiteralMismatch(const WherePredicate& p,
                                           const std::vector<Literal>& literals,
                                           core::StorageType type) {
  if (p.op.IsAnyOf<core::NullOp>() || p.op.Is<core::Glob>()) {
    return std::nullopt;
  }
  bool string_column = type.Is<core::String>();
  for (uint32_t i = 0; i < p.literal_count; ++i) {
    if (string_column != literals[p.first_literal + i].is_string()) {
      return std::string(string_column ? "a string to compare with a string "
                                         "column, but got a number"
                                       : "a number to compare with a numeric "
                                         "column, but got a string");
    }
  }
  return std::nullopt;
}

// Parses a WHERE condition into the predicates, literals and strings of a
// Where.
class Parser {
 public:
  Parser(Compiler* c,
         std::vector<WherePredicate>* predicates,
         std::vector<Literal>* literals)
      : c_(c), predicates_(predicates), literals_(literals) {}

  base::Status Condition(uint32_t expr) {
    const auto* node = Node<SyntaqliteNode>(c_->parser(), expr);
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
        return c_->Unsupported(expr, "a condition other than column OP value");
    }
  }

 private:
  base::Status Comparison(uint32_t expr, const SyntaqliteBinaryExpr& node) {
    std::optional<core::Op> op = ComparisonOp(node.op);
    if (!op) {
      return c_->Unsupported(expr, kOperators);
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.left));
    return Add(expr, column, *op, {node.right});
  }

  base::Status Like(uint32_t expr, const SyntaqliteLikeExpr& node) {
    if (node.keyword != SYNTAQLITE_LIKE_KEYWORD_GLOB) {
      return c_->Unsupported(expr, kOperators);
    }
    if (node.negated) {
      return c_->Unsupported(expr, "NOT GLOB");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    return Add(expr, column, core::Glob{}, {node.pattern});
  }

  base::Status In(uint32_t expr, const SyntaqliteInExpr& node) {
    if (node.negated) {
      return c_->Unsupported(expr, "NOT IN");
    }
    const auto* source = Node<SyntaqliteNode>(c_->parser(), node.source);
    if (node.bare_source || source->tag != SYNTAQLITE_NODE_EXPR_LIST) {
      return c_->Unsupported(expr, "IN other than a list of literals");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    const SyntaqliteExprList& list = source->expr_list;
    return Add(
        expr, column, core::In{},
        std::vector<uint32_t>(list.children, list.children + list.count));
  }

  base::Status Is(uint32_t expr, const SyntaqliteIsExpr& node) {
    ASSIGN_OR_RETURN(bool null, CompileIsNull(c_, expr, node));
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.left));
    core::Op op = null ? core::Op(core::IsNull{}) : core::Op(core::IsNotNull{});
    return Add(expr, column, op, {});
  }

  base::Status Between(uint32_t expr, const SyntaqliteBetweenExpr& node) {
    if (node.negated) {
      return c_->Unsupported(expr, "NOT BETWEEN");
    }
    ASSIGN_OR_RETURN(ColumnId column, c_->ResolveColumnExpr(node.operand));
    RETURN_IF_ERROR(Add(expr, column, core::Ge{}, {node.low}));
    return Add(expr, column, core::Le{}, {node.high});
  }

  base::Status Add(uint32_t at,
                   ColumnId column,
                   core::Op op,
                   const std::vector<uint32_t>& exprs) {
    WherePredicate predicate;
    predicate.column = column;
    predicate.op = op;
    predicate.first_literal = static_cast<uint32_t>(literals_->size());
    predicate.literal_count = static_cast<uint32_t>(exprs.size());
    for (uint32_t expr : exprs) {
      ASSIGN_OR_RETURN(Literal literal, CompileLiteral(c_, expr));
      literals_->push_back(std::move(literal));
    }
    if (std::optional<core::StorageType> type =
            c_->plan().columns()[column].type.storage()) {
      std::optional<std::string> mismatch =
          LiteralMismatch(predicate, *literals_, *type);
      if (mismatch) {
        return c_->Expected(at, *mismatch);
      }
    }
    predicates_->push_back(predicate);
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

  Compiler* c_;
  std::vector<WherePredicate>* predicates_;
  std::vector<Literal>* literals_;
};

// The type a column of no known type is converted to, for Filter to read it:
// the type its literals have. Strings win, as SQL compares a number with a
// string by type. None for IS [NOT] NULL, which reads any column.
std::optional<core::StorageType> TypeForLiterals(
    const WherePredicate& p,
    const std::vector<Literal>& literals) {
  if (p.op.IsAnyOf<core::NullOp>()) {
    return std::nullopt;
  }
  if (p.op.Is<core::Glob>()) {
    return core::String{};
  }
  core::StorageType type = core::Int64{};
  for (uint32_t i = 0; i < p.literal_count; ++i) {
    const Literal& literal = literals[p.first_literal + i];
    if (literal.is_string()) {
      return core::String{};
    }
    if (literal.is_double()) {
      type = core::Double{};
    }
  }
  return type;
}

}  // namespace

const OperationRegistration Where::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE, &BuildPlan,
    OperationRegistration::Encoding{8, &MakeOperation<Where>}};

base::Status Where::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("WHERE");
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(c->parser(), stage);
  Where where;
  Parser parser(c, &where.predicates_, &where.literals_);
  RETURN_IF_ERROR(parser.Condition(n->condition));
  c->AddNode(std::move(where), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

InputNeeds Where::Needs(uint32_t) const {
  // Filter reads only columns of one type, but for IS [NOT] NULL. A column of
  // no known type carries a type per row, so it is converted to the literals'
  // type. GLOB matches only strings.
  InputNeeds needs;
  for (const WherePredicate& p : predicates_) {
    if (std::optional<core::StorageType> wanted =
            TypeForLiterals(p, literals_)) {
      needs.types_first.push_back({p.column, *wanted, !p.op.Is<core::Glob>()});
    }
  }
  return needs;
}

OutputProperties Where::Derive(DeriveContext& c) const {
  OutputProperties out = c.input(0);
  out.DropRows();
  // A rewrite may have cast some already.
  std::vector<std::optional<ex::FilterValue>>& values = c.filter_values();
  values.resize(predicates_.size());
  for (uint32_t i = 0; i < predicates_.size(); ++i) {
    const WherePredicate& p = predicates_[i];
    std::optional<core::StorageType> type = out.TypeOf(p.column).storage();
    if (type) {
      std::optional<std::string> mismatch =
          LiteralMismatch(p, literals_, *type);
      if (mismatch) {
        c.Fail(base::ErrStatus("WHERE: expected %s", mismatch->c_str()));
        return out;
      }
    }
    if (!values[i] && type && !p.op.IsAnyOf<core::NullOp>()) {
      values[i] = CastLiterals(p.op, literals_.data() + p.first_literal,
                               p.literal_count, *type, *c.context().pool);
    }
  }
  return out;
}

void Where::Rewrite(BoundPlan* bound,
                    PlanNodeId self,
                    const LoweringContext& context) const {
  LogicalPlan& plan = bound->plan;
  PlanNode& node = plan.nodes()[self];
  PlanNodeId scan_id = node.children()[0];
  PlanNode& below = plan.nodes()[scan_id];
  if (!below.Is<Scan>() || below.Cast<Scan>().has_search()) {
    return;
  }
  auto& where = node.Cast<Where>();
  auto& scan = below.Cast<Scan>();

  // A scan converts no column, so only conditions on columns of a known type,
  // read as the table stores them, have values to search with. They are what
  // these conditions test whether or not the scan answers them.
  std::vector<std::optional<ex::FilterValue>> values(where.predicates_.size());
  std::vector<uint32_t> predicate_indices;
  std::vector<Scan::Condition> conditions;
  for (uint32_t i = 0; i < where.predicates_.size(); ++i) {
    const WherePredicate& p = where.predicates_[i];
    std::optional<core::StorageType> type =
        bound->planned[scan_id].output.TypeOf(p.column).storage();
    if (!type || p.op.IsAnyOf<core::NullOp>() ||
        (p.op.Is<core::Glob>() && !type->Is<core::String>()) ||
        LiteralMismatch(p, where.literals_, *type)) {
      continue;
    }
    values[i] = CastLiterals(p.op, where.literals_.data() + p.first_literal,
                             p.literal_count, *type, *context.pool);
    predicate_indices.push_back(i);
    conditions.push_back({p.column, p.op, &*values[i]});
  }
  std::optional<Scan::SearchPlan> searched =
      scan.PlanSearch(conditions, *bound->bindings.dataframes[scan_id]);
  if (!searched) {
    bound->planned[self].filter_values = std::move(values);
    return;
  }

  // The answered conditions move into the scan with their values.
  ScanSearch search;
  search.kind = searched->kind;
  search.index = searched->index;
  std::vector<std::optional<ex::FilterValue>>& scan_values =
      bound->planned[scan_id].filter_values;
  std::vector<bool> moved(where.predicates_.size(), false);
  for (uint32_t j : searched->answered) {
    uint32_t i = predicate_indices[j];
    moved[i] = true;
    search.predicates.push_back(where.predicates_[i]);
    scan_values.push_back(std::move(values[i]));
  }
  scan.SetSearch(std::move(search));
  PlanNodeInputs(bound, scan_id, context);
  std::vector<WherePredicate> kept;
  std::vector<std::optional<ex::FilterValue>> kept_values;
  for (uint32_t i = 0; i < where.predicates_.size(); ++i) {
    if (!moved[i]) {
      kept.push_back(where.predicates_[i]);
      kept_values.push_back(std::move(values[i]));
    }
  }
  where.predicates_ = std::move(kept);
  bound->planned[self].filter_values = std::move(kept_values);
  if (where.predicates_.empty()) {
    plan.Bypass(self);
  }
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerInput(node, 0);
  std::vector<ex::Filter::Condition> conditions;
  conditions.reserve(predicates_.size());
  for (uint32_t i = 0; i < predicates_.size(); ++i) {
    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(predicates_[i].column);
    condition.op = predicates_[i].op;
    if (std::optional<uint32_t> value = c->TakeFilterValue(node, i)) {
      condition.value = *value;
    }
    conditions.push_back(condition);
  }
  c->AddOperator(
      std::make_unique<ex::Filter>(std::move(conditions), c->context().pool));
}

const OperationRegistration& Where::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Where::Clone() const {
  return std::make_unique<Where>(*this);
}

void Where::Serialize(PlanCodec& c) {
  c.Vec(&predicates_);
  literals_.resize(c.Count(literals_.size()));
  for (Literal& literal : literals_) {
    SerializeLiteral(c, &literal);
  }
}

}  // namespace perfetto::trace_processor::pipeline
