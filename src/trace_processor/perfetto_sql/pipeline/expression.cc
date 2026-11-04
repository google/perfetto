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

#include "src/trace_processor/perfetto_sql/pipeline/expression.h"

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
#include "perfetto/ext/base/string_view.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace {

namespace ex = core::exec;
using Op = ex::ExprStep::Op;
using Type = ex::ExprType;

// Deeper expressions are refused, so compiling one cannot run out of stack.
constexpr uint32_t kMaxDepth = 256;

bool IsArithmetic(Op op) {
  return op == Op::kAdd || op == Op::kSub || op == Op::kMul || op == Op::kDiv ||
         op == Op::kMod;
}

bool IsComparison(Op op) {
  return op == Op::kEq || op == Op::kNe || op == Op::kLt || op == Op::kLe ||
         op == Op::kGt || op == Op::kGe;
}

// The comparison which holds of `x y` when `op` holds of `y x`.
Op Flipped(Op op) {
  return op == Op::kLt   ? Op::kGt
         : op == Op::kLe ? Op::kGe
         : op == Op::kGt ? Op::kLt
         : op == Op::kGe ? Op::kLe
                         : op;
}

core::Op CoreComparison(Op op) {
  if (op == Op::kEq) {
    return core::Eq{};
  }
  if (op == Op::kNe) {
    return core::Ne{};
  }
  if (op == Op::kLt) {
    return core::Lt{};
  }
  if (op == Op::kLe) {
    return core::Le{};
  }
  if (op == Op::kGt) {
    return core::Gt{};
  }
  PERFETTO_DCHECK(op == Op::kGe);
  return core::Ge{};
}

// Appends a node and returns its index.
uint32_t Push(Expr* e, ex::ExprStep node) {
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

// -----------------------------------------------------------------------------
// Compiling
// -----------------------------------------------------------------------------

// Appends a constant holding `literal`.
uint32_t PushConstant(Expr* e, Literal literal) {
  ex::ExprStep node;
  node.op = Op::kConstant;
  node.a = static_cast<uint32_t>(e->literals.size());
  e->literals.push_back(std::move(literal));
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

// Appends the nodes of the expression at `at`, its result last.
base::Status Compile(Compiler* c, uint32_t at, uint32_t depth, Expr* e) {
  if (depth > kMaxDepth) {
    return c->Unsupported(at, "an expression this deeply nested");
  }
  const auto* node = Node<SyntaqliteNode>(c->parser(), at);
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_COLUMN_REF: {
      ASSIGN_OR_RETURN(ColumnId column, c->ResolveColumnExpr(at));
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
      ASSIGN_OR_RETURN(Literal literal, CompileLiteral(c, at));
      PushConstant(e, std::move(literal));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_PAREN_EXPR:
      return Compile(c, node->paren_expr.expr, depth + 1, e);
    case SYNTAQLITE_NODE_UNARY_EXPR: {
      const SyntaqliteUnaryExpr& n = node->unary_expr;
      if (n.op != SYNTAQLITE_UNARY_OP_PLUS &&
          n.op != SYNTAQLITE_UNARY_OP_MINUS) {
        return c->Unsupported(at, "this operator");
      }
      RETURN_IF_ERROR(Compile(c, n.operand, depth + 1, e));
      if (n.op == SYNTAQLITE_UNARY_OP_PLUS) {
        return base::OkStatus();
      }
      ex::ExprStep& operand = e->nodes.back();
      if (operand.op == Op::kConstant) {
        Literal& literal = e->literals[operand.a];
        if (literal.is_string()) {
          return c->Unsupported(at, "a negated string");
        }
        // Folded, so the smallest integer can be written.
        literal.int_value = static_cast<int64_t>(
            0ull - static_cast<uint64_t>(literal.int_value));
        literal.double_value = -literal.double_value;
        return base::OkStatus();
      }
      // As in SQLite, -x is x * -1: an integer overflows only at INT64_MIN,
      // and a double zero becomes -0.0.
      auto x = static_cast<uint32_t>(e->nodes.size() - 1);
      Literal minus_one;
      minus_one.int_value = -1;
      Push(e, Node2(Op::kMul, x, PushConstant(e, std::move(minus_one))));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_LIKE_EXPR: {
      const SyntaqliteLikeExpr& n = node->like_expr;
      if (n.keyword != SYNTAQLITE_LIKE_KEYWORD_GLOB) {
        return c->Unsupported(at, "this operator");
      }
      if (n.negated) {
        return c->Unsupported(at, "NOT GLOB");
      }
      RETURN_IF_ERROR(Compile(c, n.operand, depth + 1, e));
      auto operand = static_cast<uint32_t>(e->nodes.size() - 1);
      ASSIGN_OR_RETURN(Literal pattern, CompileLiteral(c, n.pattern));
      if (!pattern.is_string()) {
        return c->Expected(n.pattern, "a string as the pattern of GLOB");
      }
      auto index = static_cast<uint32_t>(e->literals.size());
      e->literals.push_back(std::move(pattern));
      Push(e, Node2(Op::kGlob, operand, index));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_IS_EXPR: {
      const SyntaqliteIsExpr& n = node->is_expr;
      ASSIGN_OR_RETURN(bool null, CompileIsNull(c, at, n));
      RETURN_IF_ERROR(Compile(c, n.left, depth + 1, e));
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
        return c->Unsupported(at, "this operator");
      }
      RETURN_IF_ERROR(Compile(c, n.left, depth + 1, e));
      auto a = static_cast<uint32_t>(e->nodes.size() - 1);
      RETURN_IF_ERROR(Compile(c, n.right, depth + 1, e));
      auto b = static_cast<uint32_t>(e->nodes.size() - 1);
      Push(e, Node2(*op, a, b));
      return base::OkStatus();
    }
    case SYNTAQLITE_NODE_FUNCTION_CALL:
      return c->Unsupported(at, "calling functions");
    default:
      return c->Unsupported(at, "this expression");
  }
}

// -----------------------------------------------------------------------------
// Typing and lowering
// -----------------------------------------------------------------------------

// What a node computes. A bare NULL has no type of its own: it takes the type
// of whatever it meets.
enum class Kind : uint8_t { kInt64, kDouble, kString, kNull, kUnknown };

Kind KindOf(const PlanType& type) {
  if (type.is_null()) {
    return Kind::kNull;
  }
  if (!type.storage()) {
    return Kind::kUnknown;
  }
  if (type.Is<core::String>()) {
    return Kind::kString;
  }
  if (type.Is<core::Double>()) {
    return Kind::kDouble;
  }
  return Kind::kInt64;
}

Type TypeOf(Kind kind) {
  return kind == Kind::kDouble   ? Type::kDouble
         : kind == Kind::kString ? Type::kString
                                 : Type::kInt64;
}

core::StorageType StorageOf(Kind kind) {
  return kind == Kind::kDouble   ? core::StorageType(core::Double{})
         : kind == Kind::kString ? core::StorageType(core::String{})
                                 : core::StorageType(core::Int64{});
}

// Types an expression's nodes in order and, given the rows it reads, appends
// the steps computing them. Without them it only checks, as the compiler does
// before every column's type is known. An error is what was expected instead.
class Typer {
 public:
  explicit Typer(const LogicalPlan& plan) : plan_(&plan) {}
  Typer(const OutputProperties& rows,
        ex::ExprProgram* program,
        StringPool* pool)
      : rows_(&rows), program_(program), pool_(pool) {}

  // The kind of the expression's result.
  base::StatusOr<Kind> Run(const Expr& e) {
    expr_ = &e;
    for (const ex::ExprStep& node : e.nodes) {
      uint32_t reg = 0;
      ASSIGN_OR_RETURN(Kind kind, Type(node, &reg));
      kinds_.push_back(kind);
      regs_.push_back(reg);
    }
    return kinds_.back();
  }

  // The register holding node `i` as `kind`: a null of that type for a bare
  // NULL, an integer widened for a double.
  uint32_t Materialize(uint32_t i, Kind kind) {
    ex::ExprStep step;
    if (kinds_[i] == Kind::kNull) {
      step.op = Op::kNull;
      step.type = TypeOf(kind);
      return Add(step);
    }
    if (kinds_[i] == Kind::kInt64 && kind == Kind::kDouble) {
      step.op = Op::kToDouble;
      step.type = Type::kDouble;
      step.a = regs_[i];
      return Add(step);
    }
    return regs_[i];
  }

 private:
  uint32_t Add(const ex::ExprStep& step) {
    program_->steps.push_back(step);
    return static_cast<uint32_t>(program_->steps.size() - 1);
  }

  base::StatusOr<Kind> Type(const ex::ExprStep& node, uint32_t* reg) {
    if (node.op == Op::kColumn) {
      ColumnId column = node.a;
      Kind kind =
          KindOf(rows_ ? rows_->TypeOf(column) : plan_->columns()[column].type);
      if (!rows_ || kind == Kind::kNull) {
        return kind;
      }
      if (kind == Kind::kUnknown) {
        return base::ErrStatus("columns of a known type");
      }
      ex::ExprStep step;
      step.op = Op::kColumn;
      step.type = TypeOf(kind);
      step.a = *rows_->PositionOf(column);
      *reg = Add(step);
      return kind;
    }
    if (node.op == Op::kConstant) {
      const Literal& literal = expr_->literals[node.a];
      if (rows_) {
        ex::ExprStep step;
        step.op = Op::kConstant;
        step.int_value = literal.int_value;
        step.double_value = literal.double_value;
        if (literal.is_string()) {
          step.type = Type::kString;
          step.int_value =
              pool_->InternString(base::StringView(literal.text)).raw_id();
        } else if (literal.is_double()) {
          step.type = Type::kDouble;
        }
        *reg = Add(step);
      }
      return literal.is_string()   ? Kind::kString
             : literal.is_double() ? Kind::kDouble
                                   : Kind::kInt64;
    }
    if (node.op == Op::kNull) {
      return Kind::kNull;
    }
    if (IsComparison(node.op)) {
      return TypeComparison(node, reg);
    }
    if (node.op == Op::kGlob) {
      return TypeGlob(node, reg);
    }
    if (node.op == Op::kIsNull || node.op == Op::kIsNotNull) {
      if (rows_) {
        ex::ExprStep step = node;
        step.type = Type::kInt64;
        step.a =
            Materialize(node.a, kinds_[node.a] == Kind::kNull ? Kind::kInt64
                                                              : kinds_[node.a]);
        *reg = Add(step);
      }
      return Kind::kInt64;
    }
    PERFETTO_DCHECK(IsArithmetic(node.op));
    Kind a = kinds_[node.a];
    Kind b = kinds_[node.b];
    if (a == Kind::kString || b == Kind::kString) {
      return base::ErrStatus("numbers, not strings, in arithmetic");
    }
    if (node.op == Op::kMod && (a == Kind::kDouble || b == Kind::kDouble)) {
      return base::ErrStatus("integers on both sides of %%");
    }
    if (a == Kind::kUnknown || b == Kind::kUnknown) {
      return Kind::kUnknown;
    }
    // Any arithmetic with NULL is NULL.
    if (a == Kind::kNull || b == Kind::kNull) {
      return Kind::kNull;
    }
    Kind kind =
        a == Kind::kDouble || b == Kind::kDouble ? Kind::kDouble : Kind::kInt64;
    if (rows_) {
      ex::ExprStep step =
          Node2(node.op, Materialize(node.a, kind), Materialize(node.b, kind));
      step.type = TypeOf(kind);
      *reg = Add(step);
    }
    return kind;
  }

  base::StatusOr<Kind> TypeComparison(const ex::ExprStep& node, uint32_t* reg) {
    Kind a = kinds_[node.a];
    Kind b = kinds_[node.b];
    if ((a == Kind::kString && IsNumber(b)) ||
        (b == Kind::kString && IsNumber(a))) {
      return base::ErrStatus(
          "values of one kind to compare, but got a number and a string");
    }
    // A comparison with NULL is NULL.
    if (a == Kind::kNull || b == Kind::kNull) {
      return Kind::kNull;
    }
    if (a == Kind::kUnknown || b == Kind::kUnknown) {
      return Kind::kInt64;
    }
    if (a != b) {
      const ex::ExprStep* left = &expr_->nodes[node.a];
      const ex::ExprStep* right = &expr_->nodes[node.b];
      if (right->op != Op::kConstant && left->op != Op::kConstant) {
        return base::ErrStatus(
            "values of one type to compare, but got an integer and a double: "
            "multiply the integer by 1.0 to compare it as a double");
      }
      if (rows_) {
        *reg = right->op == Op::kConstant
                   ? AddLiteralComparison(node.op, node.a, *right)
                   : AddLiteralComparison(Flipped(node.op), node.b, *left);
      }
      return Kind::kInt64;
    }
    if (rows_) {
      ex::ExprStep step = Node2(node.op, regs_[node.a], regs_[node.b]);
      step.type = Type::kInt64;
      *reg = Add(step);
    }
    return Kind::kInt64;
  }

  // Adds the steps comparing node `value` with a number literal of the other
  // type.
  uint32_t AddLiteralComparison(Op op,
                                uint32_t value,
                                const ex::ExprStep& constant) {
    bool double_value = kinds_[value] == Kind::kDouble;
    ResolvedComparison resolved = ResolveLiteralComparison(
        CoreComparison(op), expr_->literals[constant.a], double_value);
    if (resolved.kind != ResolvedComparison::Kind::kCompare) {
      ex::ExprStep step;
      step.op = Op::kConstantWherePresent;
      step.a = regs_[value];
      step.int_value = resolved.kind == ResolvedComparison::Kind::kAlwaysTrue;
      return Add(step);
    }
    ex::ExprStep literal;
    literal.op = Op::kConstant;
    literal.type = double_value ? Type::kDouble : Type::kInt64;
    literal.int_value = resolved.literal.int_value;
    literal.double_value = resolved.literal.double_value;
    ex::ExprStep step = Node2(op, regs_[value], Add(literal));
    step.type = Type::kInt64;
    return Add(step);
  }

  base::StatusOr<Kind> TypeGlob(const ex::ExprStep& node, uint32_t* reg) {
    Kind a = kinds_[node.a];
    if (IsNumber(a)) {
      return base::ErrStatus("a string to match with GLOB");
    }
    // Matching NULL is NULL.
    if (a == Kind::kNull) {
      return Kind::kNull;
    }
    if (rows_) {
      ex::ExprStep step = node;
      step.type = Type::kInt64;
      step.a = regs_[node.a];
      step.b = static_cast<uint32_t>(program_->patterns.size());
      program_->patterns.push_back(expr_->literals[node.b].text);
      *reg = Add(step);
    }
    return Kind::kInt64;
  }

  static bool IsNumber(Kind kind) {
    return kind == Kind::kInt64 || kind == Kind::kDouble;
  }

  const LogicalPlan* plan_ = nullptr;
  const OutputProperties* rows_ = nullptr;
  ex::ExprProgram* program_ = nullptr;
  StringPool* pool_ = nullptr;
  const Expr* expr_ = nullptr;
  // By node, its kind and the register holding it.
  std::vector<Kind> kinds_;
  std::vector<uint32_t> regs_;
};

}  // namespace

base::StatusOr<bool> CompileIsNull(Compiler* c,
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
      const auto* right = Node<SyntaqliteNode>(c->parser(), node.right);
      if (right->tag != SYNTAQLITE_NODE_LITERAL ||
          right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
        return c->Unsupported(expr, "IS other than IS [NOT] NULL");
      }
      return node.op == SYNTAQLITE_IS_OP_IS;
    }
    default:
      return c->Unsupported(expr, "IS other than IS [NOT] NULL");
  }
}

base::Status CompileExpr(Compiler* c, uint32_t expr, Expr* out) {
  return Compile(c, expr, 0, out);
}

base::StatusOr<PlanType> CheckExprType(const Compiler& c, const Expr& e) {
  ASSIGN_OR_RETURN(Kind kind, Typer(c.plan()).Run(e));
  if (kind == Kind::kUnknown) {
    return PlanType::Unknown();
  }
  return PlanType(StorageOf(kind));
}

void MarkExprColumns(const Expr& e, std::vector<bool>* needed) {
  for (const ex::ExprStep& node : e.nodes) {
    if (node.op == Op::kColumn) {
      (*needed)[node.a] = true;
    }
  }
}

base::StatusOr<TypedExpr> TypeExpr(const OutputProperties& rows,
                                   const Expr& e,
                                   StringPool* pool) {
  TypedExpr typed;
  Typer typer(rows, &typed.program, pool);
  base::StatusOr<Kind> kind = typer.Run(e);
  if (!kind.ok()) {
    return base::ErrStatus("expected %s", kind.status().c_message());
  }
  // A result which is only ever NULL is a column of Int64 nulls.
  Kind result = *kind == Kind::kNull ? Kind::kInt64 : *kind;
  uint32_t reg =
      typer.Materialize(static_cast<uint32_t>(e.nodes.size() - 1), result);
  if (reg != typed.program.steps.size() - 1) {
    // The result is an earlier step, such as a bare column: repeat it last.
    ex::ExprStep step = typed.program.steps[reg];
    typed.program.steps.push_back(step);
  }
  typed.type = StorageOf(result);
  return typed;
}

void SerializeExpr(PlanCodec& c, Expr* e) {
  // A step has padding, so its fields are copied one at a time.
  e->nodes.resize(c.Count(e->nodes.size()));
  for (ex::ExprStep& node : e->nodes) {
    c.Pod(&node.op);
    c.Pod(&node.type);
    c.Pod(&node.a);
    c.Pod(&node.b);
    c.Pod(&node.int_value);
    c.Pod(&node.double_value);
  }
  e->literals.resize(c.Count(e->literals.size()));
  for (Literal& literal : e->literals) {
    SerializeLiteral(c, &literal);
  }
}

}  // namespace perfetto::trace_processor::pipeline
