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
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
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

base::Status CompileLiteral(Compiler* c,
                            uint32_t at,
                            const SyntaqliteLiteral& literal,
                            Expr* e) {
  ex::ExprStep node;
  node.op = Op::kConstant;
  std::string text = SpanText(c->parser(), literal.source);
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
        // SQLite reads it as a double instead.
        return c->Unsupported(at, "an integer literal too large for 64 bits");
      }
      node.int_value = *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
      std::optional<double> value = base::StringToDouble(text);
      if (!value) {
        return c->Expected(at, "a number");
      }
      node.type = Type::kDouble;
      node.double_value = *value;
      break;
    }
    case SYNTAQLITE_LITERAL_TYPE_NULL:
      node.op = Op::kNull;
      break;
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      return c->Unsupported(at, "a string literal");
    default:
      return c->Unsupported(at, "this literal");
  }
  Push(e, node);
  return base::OkStatus();
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
    case SYNTAQLITE_NODE_LITERAL:
      return CompileLiteral(c, at, node->literal, e);
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
        // Folded, so the smallest integer can be written.
        operand.int_value = static_cast<int64_t>(
            0ull - static_cast<uint64_t>(operand.int_value));
        operand.double_value = -operand.double_value;
        return base::OkStatus();
      }
      // As in SQLite, -x is x * -1: an integer overflows only at INT64_MIN,
      // and a double zero becomes -0.0.
      auto x = static_cast<uint32_t>(e->nodes.size() - 1);
      ex::ExprStep minus_one;
      minus_one.op = Op::kConstant;
      minus_one.int_value = -1;
      Push(e, Node2(Op::kMul, x, Push(e, minus_one)));
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

// Types an expression's nodes in order and, when lowering, appends the steps
// computing them. Without a Lowering it only checks, as the compiler does
// before every column's type is known. An error is what was expected
// instead.
class Typer {
 public:
  Typer(const LogicalPlan& plan, Lowering* lowering, ex::ExprProgram* program)
      : plan_(plan), lowering_(lowering), program_(program) {}

  // The kind of the expression's result.
  base::StatusOr<Kind> Run(const Expr& e) {
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
      Kind kind = KindOf(lowering_ ? lowering_->QueryType(column)
                                   : plan_.columns()[column].type);
      if (!lowering_ || kind == Kind::kNull) {
        return kind;
      }
      if (kind == Kind::kUnknown) {
        return base::ErrStatus("columns of a known type");
      }
      ex::ExprStep step;
      step.op = Op::kColumn;
      step.type = TypeOf(kind);
      step.a = lowering_->QueryPosition(column);
      *reg = Add(step);
      return kind;
    }
    if (node.op == Op::kConstant) {
      if (lowering_) {
        *reg = Add(node);
      }
      return node.type == Type::kDouble ? Kind::kDouble : Kind::kInt64;
    }
    if (node.op == Op::kNull) {
      return Kind::kNull;
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
    if (lowering_) {
      ex::ExprStep step =
          Node2(node.op, Materialize(node.a, kind), Materialize(node.b, kind));
      step.type = TypeOf(kind);
      *reg = Add(step);
    }
    return kind;
  }

  const LogicalPlan& plan_;
  Lowering* lowering_;
  ex::ExprProgram* program_;
  // By node, its kind and, when lowering, the register holding it.
  std::vector<Kind> kinds_;
  std::vector<uint32_t> regs_;
};

}  // namespace

base::Status CompileExpr(Compiler* c, uint32_t expr, Expr* out) {
  return Compile(c, expr, 0, out);
}

base::StatusOr<PlanType> CheckExprType(const Compiler& c, const Expr& e) {
  ASSIGN_OR_RETURN(Kind kind, Typer(c.plan(), nullptr, nullptr).Run(e));
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

core::exec::ExprProgram LowerExpr(Lowering* c,
                                  const Expr& e,
                                  core::StorageType* type) {
  ex::ExprProgram program;
  Typer typer(c->plan(), c, &program);
  base::StatusOr<Kind> kind = typer.Run(e);
  if (!kind.ok()) {
    c->Fail(base::ErrStatus("expected %s", kind.status().c_message()));
    *type = core::Int64{};
    return program;
  }
  // A result which is only ever NULL is a column of Int64 nulls.
  Kind result = *kind == Kind::kNull ? Kind::kInt64 : *kind;
  uint32_t reg =
      typer.Materialize(static_cast<uint32_t>(e.nodes.size() - 1), result);
  if (reg != program.steps.size() - 1) {
    // The result is an earlier step, such as a bare column: repeat it last.
    ex::ExprStep step = program.steps[reg];
    program.steps.push_back(step);
  }
  *type = StorageOf(result);
  return program;
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
}

}  // namespace perfetto::trace_processor::pipeline
