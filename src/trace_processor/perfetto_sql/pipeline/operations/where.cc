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
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// Hands a WHERE literal to the shared filter value casts, so it means for
// each column type what any other filter value means.
struct LiteralFetcher : core::ValueFetcher {
  static constexpr Type kInt64 = 0;
  static constexpr Type kDouble = 1;
  static constexpr Type kString = 2;
  static constexpr Type kNull = 3;

  explicit LiteralFetcher(const WherePredicate& p) : predicate(p) {}

  Type GetValueType(uint32_t) const {
    return predicate.is_double ? kDouble : kInt64;
  }
  int64_t GetInt64Value(uint32_t) const { return predicate.int_value; }
  double GetDoubleValue(uint32_t) const { return predicate.double_value; }
  const char* GetStringValue(uint32_t) const {
    PERFETTO_FATAL("WHERE literals are numbers");
  }

  const WherePredicate& predicate;
};

// Reads a number literal, optionally negated, into `out`.
base::Status ParseLiteral(Compiler* c, uint32_t expr, WherePredicate* out) {
  SyntaqliteParser* p = c->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL) {
    return c->Unsupported(expr, "a value other than a number literal");
  }
  const SyntaqliteLiteral& literal = node->literal;
  std::string text = SpanText(p, literal.source);
  if (literal.literal_type == SYNTAQLITE_LITERAL_TYPE_INTEGER) {
    std::optional<int64_t> value = base::StringToInt64(text);
    if (!value) {
      return c->Unsupported(expr, "a number which does not fit an integer");
    }
    out->is_double = false;
    out->int_value = negate ? -*value : *value;
    return base::OkStatus();
  }
  if (literal.literal_type == SYNTAQLITE_LITERAL_TYPE_FLOAT) {
    std::optional<double> value = base::StringToDouble(text);
    if (!value) {
      return c->Unsupported(expr, "a malformed number");
    }
    out->is_double = true;
    out->double_value = negate ? -*value : *value;
    return base::OkStatus();
  }
  return c->Unsupported(expr, "a literal other than a number");
}

base::StatusOr<WherePredicate> ParsePredicate(Compiler* c, uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(c->parser(), expr);
  if (node->tag != SYNTAQLITE_NODE_BINARY_EXPR) {
    return c->Unsupported(expr, "a condition other than column OP value");
  }
  WherePredicate predicate;
  SyntaqliteBinaryOp op = node->binary_expr.op;
  if (op == SYNTAQLITE_BINARY_OP_EQ) {
    predicate.op = core::Eq{};
  } else if (op == SYNTAQLITE_BINARY_OP_NE) {
    predicate.op = core::Ne{};
  } else if (op == SYNTAQLITE_BINARY_OP_LT) {
    predicate.op = core::Lt{};
  } else if (op == SYNTAQLITE_BINARY_OP_LE) {
    predicate.op = core::Le{};
  } else if (op == SYNTAQLITE_BINARY_OP_GT) {
    predicate.op = core::Gt{};
  } else if (op == SYNTAQLITE_BINARY_OP_GE) {
    predicate.op = core::Ge{};
  } else {
    return c->Unsupported(expr, "an operator other than =, !=, <, <=, >, >=");
  }
  ASSIGN_OR_RETURN(predicate.column,
                   c->ResolveColumnExpr(node->binary_expr.left));
  RETURN_IF_ERROR(ParseLiteral(c, node->binary_expr.right, &predicate));
  return predicate;
}

base::Status ParseCondition(Compiler* c,
                            uint32_t expr,
                            std::vector<WherePredicate>* out) {
  const auto* node = Node<SyntaqliteNode>(c->parser(), expr);
  if (node->tag == SYNTAQLITE_NODE_BINARY_EXPR &&
      node->binary_expr.op == SYNTAQLITE_BINARY_OP_AND) {
    RETURN_IF_ERROR(ParseCondition(c, node->binary_expr.left, out));
    return ParseCondition(c, node->binary_expr.right, out);
  }
  ASSIGN_OR_RETURN(WherePredicate predicate, ParsePredicate(c, expr));
  out->push_back(predicate);
  return base::OkStatus();
}

}  // namespace

const OperationRegistration Where::kRegistration{
    SYNTAQLITE_NODE_PERFETTO_PIPE_WHERE, &BuildPlan,
    OperationRegistration::Encoding{8, false, &DecodePlan}};

base::Status Where::BuildPlan(Compiler* c, uint32_t stage) {
  c->SetOperation("WHERE");
  const auto* n = Node<SyntaqlitePerfettoPipeWhere>(c->parser(), stage);
  Where where;
  if (!ParseCondition(c, n->condition, &where.predicates_).ok()) {
    where.predicates_.clear();
    ASSIGN_OR_RETURN(Expr condition, CompileExpr(c, n->condition));
    base::StatusOr<PlanType> type = CheckExprType(c, condition);
    if (!type.ok()) {
      return c->Expected(n->condition, type.status().message());
    }
    if (type->Is<core::String>() || type->Is<core::Double>()) {
      return c->Expected(n->condition, "an integer truth value");
    }
    where.condition_ = std::move(condition);
  }
  c->AddNode(std::move(where), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  if (condition_) {
    MarkExprColumns(*condition_, needed);
  }
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  if (condition_) {
    core::StorageType type = core::Int64{};
    ex::ExprProgram program =
        LowerExpr(c, *condition_, c->context().pool, &type);
    if (!type.Is<core::Int64>()) {
      c->Fail(base::ErrStatus("WHERE: expected an integer truth value"));
      return;
    }
    c->AddRowLocal(std::make_unique<ex::Where>(std::move(program)));
    c->DeclareSmallBatches();
    return;
  }
  std::vector<ex::Filter::Condition> conditions;
  for (const WherePredicate& p : predicates_) {
    // Filter reads only columns of one type. A column of no known type
    // carries a type per row, so it is converted to the literal's.
    std::optional<core::StorageType> type = c->QueryType(p.column).storage();
    if (!type) {
      type = p.is_double ? core::StorageType(core::Double{})
                         : core::StorageType(core::Int64{});
      c->RequireType(p.column, *type);
    }
    LiteralFetcher fetcher(p);
    ex::FilterValue value;
    ex::GetFilterValueCaster<LiteralFetcher>(*type, p.op)(
        fetcher, 0, *c->context().pool, &value);

    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(p.column);
    condition.op = p.op;
    condition.value = c->AddFilterValue(std::move(value));
    conditions.push_back(condition);
  }
  c->AddOperator(
      std::make_unique<ex::Filter>(std::move(conditions), c->context().pool));
  c->DeclareSmallBatches();
}

const OperationRegistration& Where::registration() const {
  return kRegistration;
}

std::unique_ptr<PlanOperation> Where::Clone() const {
  return std::make_unique<Where>(*this);
}

void Where::Write(PlanWriter* c, const PlanNode&, Available* available) const {
  c->writer().U8(condition_ ? 1 : 0);
  if (condition_) {
    WriteExpr(c, *condition_, *available);
    return;
  }
  c->writer().Size(predicates_.size());
  for (const WherePredicate& p : predicates_) {
    c->writer().Position(*available, p.column);
    c->writer().U8(static_cast<uint8_t>(p.op.index()));
    c->writer().U8(p.is_double ? 1 : 0);
    if (p.is_double) {
      c->writer().F64(p.double_value);
    } else {
      c->writer().U64(static_cast<uint64_t>(p.int_value));
    }
  }
}

void Where::DecodePlan(PlanReader* c, Available* available) {
  Where where;
  if (c->reader().U8() != 0) {
    where.condition_ = ReadExpr(c, *available);
    c->AddNode(std::move(where), {c->plan().root()});
    return;
  }
  where.predicates_.resize(c->reader().Count());
  if (where.predicates_.empty()) {
    c->reader().Fail();
  }
  for (WherePredicate& p : where.predicates_) {
    p.column = c->reader().Position(*available);
    // Only comparisons are written; anything else is corrupt.
    std::optional<core::Op> op = core::Op::FromIndex(c->reader().U8());
    if (!op || !op->IsAnyOf<core::NonStringOp>()) {
      c->reader().Fail();
      continue;
    }
    p.op = *op;
    p.is_double = c->reader().U8() != 0;
    if (p.is_double) {
      p.double_value = c->reader().F64();
    } else {
      p.int_value = static_cast<int64_t>(c->reader().U64());
    }
  }
  c->AddNode(std::move(where), {c->plan().root()});
}

}  // namespace perfetto::trace_processor::pipeline
