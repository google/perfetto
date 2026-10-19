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

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace ex = core::exec;
namespace {

// A literal number, parsed from the AST.
struct Literal {
  bool is_double = false;
  int64_t int_value = 0;
  double double_value = 0;
};

base::StatusOr<Literal> ParseLiteral(Compiler* c, uint32_t expr) {
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
    return Literal{false, negate ? -*value : *value, 0};
  }
  if (literal.literal_type == SYNTAQLITE_LITERAL_TYPE_FLOAT) {
    std::optional<double> value = base::StringToDouble(text);
    if (!value) {
      return c->Unsupported(expr, "a malformed number");
    }
    return Literal{true, 0, negate ? -*value : *value};
  }
  return c->Unsupported(expr, "a literal other than a number");
}

base::StatusOr<WherePredicate> ParsePredicate(Compiler* c, uint32_t expr) {
  const auto* node = Node<SyntaqliteNode>(c->parser(), expr);
  if (node->tag != SYNTAQLITE_NODE_BINARY_EXPR) {
    return c->Unsupported(expr, "a condition other than column OP value");
  }
  core::Op op{core::Eq{}};
  SyntaqliteBinaryOp binary_op = node->binary_expr.op;
  if (binary_op == SYNTAQLITE_BINARY_OP_EQ) {
    op = core::Eq{};
  } else if (binary_op == SYNTAQLITE_BINARY_OP_NE) {
    op = core::Ne{};
  } else if (binary_op == SYNTAQLITE_BINARY_OP_LT) {
    op = core::Lt{};
  } else if (binary_op == SYNTAQLITE_BINARY_OP_LE) {
    op = core::Le{};
  } else if (binary_op == SYNTAQLITE_BINARY_OP_GT) {
    op = core::Gt{};
  } else if (binary_op == SYNTAQLITE_BINARY_OP_GE) {
    op = core::Ge{};
  } else {
    return c->Unsupported(expr, "an operator other than =, !=, <, <=, >, >=");
  }
  ASSIGN_OR_RETURN(ColumnId column,
                   c->ResolveColumnExpr(node->binary_expr.left));
  const PlanType& type = c->plan().columns()[column].type;
  if (type.storage() && type.Is<core::String>()) {
    return c->Unsupported(expr, "filtering by a string column");
  }
  ASSIGN_OR_RETURN(Literal literal, ParseLiteral(c, node->binary_expr.right));

  bool is_double;
  if (type.storage()) {
    is_double = type.Is<core::Double>();
    if (!is_double && literal.is_double) {
      return c->Unsupported(
          expr, "comparing a whole number column to a fractional literal");
    }
  } else {
    is_double = literal.is_double;
  }

  WherePredicate predicate;
  predicate.column = column;
  predicate.op = op;
  predicate.is_double = is_double;
  if (is_double) {
    predicate.double_value = literal.is_double
                                 ? literal.double_value
                                 : static_cast<double>(literal.int_value);
  } else {
    predicate.int_value = literal.int_value;
  }
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
  RETURN_IF_ERROR(ParseCondition(c, n->condition, &where.predicates_));
  c->AddNode(std::move(where), {c->plan().root()});
  return base::OkStatus();
}

std::optional<uint32_t> Where::Prune(std::vector<bool>* needed) {
  for (const WherePredicate& p : predicates_) {
    (*needed)[p.column] = true;
  }
  return std::nullopt;
}

void Where::Lower(Lowering* c, const PlanNode& node) const {
  c->LowerNode(node.children()[0]);
  std::vector<ex::Filter::Condition> conditions;
  for (const WherePredicate& p : predicates_) {
    if (p.is_double) {
      c->RequireType(p.column, core::Double{});
    } else {
      c->RequireType(p.column, core::Int64{});
    }
    ex::FilterValue filter_value;
    filter_value.value =
        p.is_double ? core::filter::CastFilterValueResult::Valid(p.double_value)
                    : core::filter::CastFilterValueResult::Valid(p.int_value);
    uint32_t value = c->AddFilterValue(std::move(filter_value));
    ex::Filter::Condition condition;
    condition.column = c->QueryPosition(p.column);
    condition.op = p.op;
    condition.value = value;
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
