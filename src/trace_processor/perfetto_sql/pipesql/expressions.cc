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

#include "src/trace_processor/perfetto_sql/pipesql/expressions.h"

#include <cstdint>
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
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/perfetto_sql/pipesql/stages.h"

namespace perfetto::trace_processor::pipesql {
namespace {

namespace ex = core::exec;
using Op = ex::ExprStep::Op;
using pipeline::ColumnId;
using pipeline::Literal;

// Deeper expressions are refused so recursion cannot overflow the stack.
constexpr uint32_t kMaxDepth = 256;

struct Draft {
  std::vector<ex::ExprStep> nodes;
  std::vector<Literal> literals;
};

int64_t NegateInt64(int64_t value) {
  return static_cast<int64_t>(0ull - static_cast<uint64_t>(value));
}

std::string StringLiteralText(const std::string& source) {
  PERFETTO_CHECK(source.size() >= 2 && source.front() == '\'' &&
                 source.back() == '\'');
  return base::ReplaceAll(source.substr(1, source.size() - 2), "''", "'");
}

uint32_t Push(Draft* e, ex::ExprStep node) {
  e->nodes.push_back(node);
  return static_cast<uint32_t>(e->nodes.size() - 1);
}

ex::ExprStep Node2(Op op, uint32_t a, uint32_t b) {
  ex::ExprStep node;
  node.op = op;
  node.a = a;
  node.b = b;
  return node;
}

uint32_t PushConstant(Draft* e, Literal literal) {
  ex::ExprStep node;
  node.op = Op::kConstant;
  node.a = static_cast<uint32_t>(e->literals.size());
  e->literals.push_back(literal);
  return Push(e, node);
}

std::optional<Op> BinaryOp(SyntaqliteBinaryOp op) {
  switch (static_cast<int>(op)) {
    case SYNTAQLITE_BINARY_OP_PLUS:
      return Op::kAdd;
    case SYNTAQLITE_BINARY_OP_MINUS:
      return Op::kSub;
    case SYNTAQLITE_BINARY_OP_STAR:
      return Op::kMul;
    case SYNTAQLITE_BINARY_OP_SLASH:
      return Op::kDiv;
    case SYNTAQLITE_BINARY_OP_REM:
      return Op::kMod;
    case SYNTAQLITE_BINARY_OP_EQ:
    case SYNTAQLITE_BINARY_OP_EQ_DOUBLE:
      return Op::kEq;
    case SYNTAQLITE_BINARY_OP_NE:
    case SYNTAQLITE_BINARY_OP_NE_ANGLE:
      return Op::kNe;
    case SYNTAQLITE_BINARY_OP_LT:
      return Op::kLt;
    case SYNTAQLITE_BINARY_OP_LE:
      return Op::kLe;
    case SYNTAQLITE_BINARY_OP_GT:
      return Op::kGt;
    case SYNTAQLITE_BINARY_OP_GE:
      return Op::kGe;
    default:
      return std::nullopt;
  }
}

base::Status TranslateExprNode(Translator* t,
                               uint32_t at,
                               uint32_t depth,
                               Draft* e);

base::Status TranslateFunction(Translator* t,
                               uint32_t at,
                               const SyntaqliteFunctionCall& n,
                               uint32_t depth,
                               Draft* e) {
  std::string name = base::ToLower(SpanText(t->parser(), n.func_name));
  bool iif = name == "iif";
  bool ifnull = name == "ifnull";
  if (!iif && !ifnull && name != "coalesce") {
    return t->Unsupported(at, "calling this function");
  }
  if (n.flags.bits.distinct || n.flags.bits.star ||
      syntaqlite_node_is_present(n.filter_clause) ||
      syntaqlite_node_is_present(n.over_clause) ||
      !syntaqlite_node_is_present(n.args)) {
    return t->Unsupported(at, "this form of " + name);
  }
  const auto& args = Node<SyntaqliteNode>(t->parser(), n.args)->expr_list;
  if (iif && args.count != 3) {
    return t->Expected(at, "iif to have 3 arguments");
  }
  if (ifnull && args.count != 2) {
    return t->Expected(at, "ifnull to have 2 arguments");
  }
  if (args.count < 2) {
    return t->Expected(at, "coalesce to have at least 2 arguments");
  }
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < args.count; ++i) {
    RETURN_IF_ERROR(TranslateExprNode(t, args.children[i], depth + 1, e));
    values.push_back(static_cast<uint32_t>(e->nodes.size() - 1));
  }
  if (iif) {
    ex::ExprStep node = Node2(Op::kIif, values[0], values[1]);
    node.c = values[2];
    Push(e, node);
    return base::OkStatus();
  }
  uint32_t rest = values.back();
  for (uint32_t i = args.count - 1; i-- > 0;) {
    rest = Push(e, Node2(Op::kCoalesce, values[i], rest));
  }
  return base::OkStatus();
}

base::Status TranslateExprNode(Translator* t,
                               uint32_t at,
                               uint32_t depth,
                               Draft* e) {
  if (depth > kMaxDepth) {
    return t->Unsupported(at, "an expression this deeply nested");
  }
  const auto* node = Node<SyntaqliteNode>(t->parser(), at);
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_COLUMN_REF: {
      ASSIGN_OR_RETURN(ColumnId column, t->ResolveColumnExpr(at));
      ex::ExprStep read;
      read.op = Op::kColumn;
      read.a = column;
      Push(e, read);
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_LITERAL: {
      if (node->literal.literal_type == SYNTAQLITE_LITERAL_TYPE_NULL) {
        ex::ExprStep null;
        null.op = Op::kNull;
        Push(e, null);
        return base::OkStatus();
      }
      ASSIGN_OR_RETURN(Literal literal, TranslateLiteral(t, at));
      PushConstant(e, literal);
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_PAREN_EXPR:
      return TranslateExprNode(t, node->paren_expr.expr, depth + 1, e);
    case SYNTAQLITE_NODE_UNARY_EXPR: {
      const SyntaqliteUnaryExpr& n = node->unary_expr;
      if (n.op != SYNTAQLITE_UNARY_OP_PLUS &&
          n.op != SYNTAQLITE_UNARY_OP_MINUS) {
        return t->Unsupported(at, "this operator");
      }
      RETURN_IF_ERROR(TranslateExprNode(t, n.operand, depth + 1, e));
      if (n.op == SYNTAQLITE_UNARY_OP_PLUS) {
        return base::OkStatus();
      }
      ex::ExprStep& operand = e->nodes.back();
      if (operand.op == Op::kConstant) {
        Literal& literal = e->literals[operand.a];
        if (literal.is_string()) {
          return t->Unsupported(at, "a negated string");
        }
        // Folded so INT64_MIN can be written without overflowing positive int.
        literal.int_value = NegateInt64(literal.int_value);
        literal.double_value = -literal.double_value;
        return base::OkStatus();
      }
      // As in SQLite, -x is x * -1 (overflows only at INT64_MIN, -0.0 kept).
      auto x = static_cast<uint32_t>(e->nodes.size() - 1);
      Push(e, Node2(Op::kMul, x, PushConstant(e, Literal::Int64(-1))));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_LIKE_EXPR: {
      const SyntaqliteLikeExpr& n = node->like_expr;
      if (n.keyword != SYNTAQLITE_LIKE_KEYWORD_GLOB) {
        return t->Unsupported(at, "this operator");
      }
      if (n.negated) {
        return t->Unsupported(at, "NOT GLOB");
      }
      RETURN_IF_ERROR(TranslateExprNode(t, n.operand, depth + 1, e));
      auto operand = static_cast<uint32_t>(e->nodes.size() - 1);
      ASSIGN_OR_RETURN(Literal pattern, TranslateLiteral(t, n.pattern));
      if (!pattern.is_string()) {
        return t->Expected(n.pattern, "a string as the pattern of GLOB");
      }
      auto index = static_cast<uint32_t>(e->literals.size());
      e->literals.push_back(pattern);
      Push(e, Node2(Op::kGlob, operand, index));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_IS_EXPR: {
      const SyntaqliteIsExpr& n = node->is_expr;
      ASSIGN_OR_RETURN(bool null, TranslateIsNull(t, at, n));
      RETURN_IF_ERROR(TranslateExprNode(t, n.left, depth + 1, e));
      auto operand = static_cast<uint32_t>(e->nodes.size() - 1);
      ex::ExprStep test;
      test.op = null ? Op::kIsNull : Op::kIsNotNull;
      test.a = operand;
      Push(e, test);
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_BINARY_EXPR: {
      const SyntaqliteBinaryExpr& n = node->binary_expr;
      std::optional<Op> op = BinaryOp(n.op);
      if (!op) {
        return t->Unsupported(at, "this operator");
      }
      RETURN_IF_ERROR(TranslateExprNode(t, n.left, depth + 1, e));
      auto a = static_cast<uint32_t>(e->nodes.size() - 1);
      RETURN_IF_ERROR(TranslateExprNode(t, n.right, depth + 1, e));
      auto b = static_cast<uint32_t>(e->nodes.size() - 1);
      Push(e, Node2(*op, a, b));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_FUNCTION_CALL:
      return TranslateFunction(t, at, node->function_call, depth, e);
    default:
      return t->Unsupported(at, "this expression");
  }
}

}  // namespace

base::StatusOr<int64_t> TranslateInt64Literal(Translator* t, uint32_t expr) {
  SyntaqliteParser* p = t->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL ||
      node->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_INTEGER) {
    return t->Unsupported(expr, "a value other than an integer literal");
  }
  std::optional<int64_t> value =
      base::StringToInt64(SpanText(p, node->literal.source));
  if (!value) {
    return t->Unsupported(expr, "a number which does not fit an integer");
  }
  return negate ? NegateInt64(*value) : *value;
}

base::StatusOr<pipeline::Literal> TranslateLiteral(Translator* t,
                                                   uint32_t expr) {
  SyntaqliteParser* p = t->parser();
  const auto* node = Node<SyntaqliteNode>(p, expr);
  bool negate = false;
  if (node->tag == SYNTAQLITE_NODE_UNARY_EXPR &&
      node->unary_expr.op == SYNTAQLITE_UNARY_OP_MINUS) {
    negate = true;
    node = Node<SyntaqliteNode>(p, node->unary_expr.operand);
  }
  if (node->tag != SYNTAQLITE_NODE_LITERAL) {
    return t->Unsupported(expr, "a value other than a literal");
  }
  const SyntaqliteLiteral& literal = node->literal;
  std::string text = SpanText(p, literal.source);
  switch (static_cast<int>(literal.literal_type)) {
    case SYNTAQLITE_LITERAL_TYPE_INTEGER: {
      std::optional<int64_t> value = base::StringToInt64(text);
      if (!value && base::StartsWith(base::ToLower(text), "0x")) {
        std::optional<uint64_t> bits = base::StringToUInt64(text.substr(2), 16);
        if (bits) {
          value = static_cast<int64_t>(*bits);
        }
      }
      if (!value) {
        return t->Unsupported(expr, "a number which does not fit an integer");
      }
      return Literal::Int64(negate ? NegateInt64(*value) : *value);
    }
    case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
      std::optional<double> value = base::StringToDouble(text);
      if (!value) {
        return t->Unsupported(expr, "a malformed number");
      }
      return Literal::Double(negate ? -*value : *value);
    }
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      if (negate) {
        return t->Unsupported(expr, "a negated string");
      }
      return t->builder().StringLiteral(StringLiteralText(text));
    default:
      return t->Unsupported(expr, "a literal other than a number or string");
  }
}

base::StatusOr<bool> TranslateIsNull(Translator* t,
                                     uint32_t expr,
                                     const SyntaqliteIsExpr& node) {
  switch (static_cast<int>(node.op)) {
    case SYNTAQLITE_IS_OP_IS_NULL:
      return true;
    case SYNTAQLITE_IS_OP_NOT_NULL:
    case SYNTAQLITE_IS_OP_NOT_NULL_SPACED:
      return false;
    case SYNTAQLITE_IS_OP_IS:
    case SYNTAQLITE_IS_OP_IS_NOT: {
      const auto* right = Node<SyntaqliteNode>(t->parser(), node.right);
      if (right->tag != SYNTAQLITE_NODE_LITERAL ||
          right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
        return t->Unsupported(expr, "IS other than IS [NOT] NULL");
      }
      return node.op == SYNTAQLITE_IS_OP_IS;
    }
    default:
      return t->Unsupported(expr, "IS other than IS [NOT] NULL");
  }
}

base::Status TranslateExpr(Translator* t, uint32_t expr, pipeline::Expr* out) {
  Draft draft;
  RETURN_IF_ERROR(TranslateExprNode(t, expr, 0, &draft));
  *out = t->builder().MakeExpr(draft.nodes, draft.literals);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::pipesql
