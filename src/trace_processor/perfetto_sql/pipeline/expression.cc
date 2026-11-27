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
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace {

namespace ex = core::exec;
using Op = ex::ExprStep::Op;
using Type = ex::ExprType;

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

ex::ExprStep Node2(Op op, uint32_t a, uint32_t b) {
  ex::ExprStep node;
  node.op = op;
  node.a = a;
  node.b = b;
  return node;
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
      can_fail_.push_back(CanFail(node, kind));
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
      PERFETTO_DCHECK(!literal.is_given());
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
    if (node.op == Op::kCoalesce) {
      return TypeChoice(node, "coalesce", node.a, node.b, reg);
    }
    if (node.op == Op::kIif) {
      return TypeChoice(node, "iif", node.b, node.c, reg);
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

  // Whether computing `node` can fail: only integer arithmetic can.
  bool CanFail(const ex::ExprStep& node, Kind kind) const {
    if (IsArithmetic(node.op)) {
      return kind == Kind::kInt64 || can_fail_[node.a] || can_fail_[node.b];
    }
    if (IsComparison(node.op)) {
      return can_fail_[node.a] || can_fail_[node.b];
    }
    bool reads_a = node.op == Op::kGlob || node.op == Op::kIsNull ||
                   node.op == Op::kIsNotNull || node.op == Op::kCoalesce ||
                   node.op == Op::kIif;
    return reads_a && can_fail_[node.a];
  }

  static const char* KindName(Kind kind) {
    return kind == Kind::kDouble   ? "double"
           : kind == Kind::kString ? "string"
                                   : "integer";
  }

  // A coalesce or iif, choosing between nodes `x` and `y`. Every step runs for
  // every row, so one only some rows choose must not be able to fail.
  base::StatusOr<Kind> TypeChoice(const ex::ExprStep& node,
                                  const char* name,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t* reg) {
    bool iif = node.op == Op::kIif;
    if ((iif && can_fail_[x]) || can_fail_[y]) {
      return base::ErrStatus(
          "no integer arithmetic in the arguments of %s which only some rows "
          "use, as it could overflow on the others",
          name);
    }
    if (iif && kinds_[node.a] == Kind::kString) {
      return base::ErrStatus("a number as the condition of iif");
    }
    Kind a = kinds_[x];
    Kind b = kinds_[y];
    if (a != Kind::kNull && b != Kind::kNull && a != Kind::kUnknown &&
        b != Kind::kUnknown && a != b) {
      return base::ErrStatus(
          "the arguments of %s to have one type, but got %s and %s", name,
          KindName(a), KindName(b));
    }
    Kind kind = a == Kind::kNull || a == Kind::kUnknown ? b : a;
    if (!rows_) {
      return kind;
    }
    // A result which is only ever NULL is a column of Int64 nulls.
    Kind typed = kind == Kind::kNull ? Kind::kInt64 : kind;
    ex::ExprStep step;
    step.op = node.op;
    step.type = TypeOf(typed);
    if (iif) {
      step.a = AddCondition(node.a);
      step.b = Materialize(x, typed);
      step.c = Materialize(y, typed);
    } else {
      step.a = Materialize(x, typed);
      step.b = Materialize(y, typed);
    }
    *reg = Add(step);
    return kind;
  }

  // The register holding node `i` as an integer condition. A double is true
  // unless it is zero.
  uint32_t AddCondition(uint32_t i) {
    if (kinds_[i] != Kind::kDouble) {
      return Materialize(i, Kind::kInt64);
    }
    ex::ExprStep zero;
    zero.op = Op::kConstant;
    zero.type = Type::kDouble;
    ex::ExprStep step = Node2(Op::kNe, regs_[i], Add(zero));
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
      program_->patterns.emplace_back(expr_->literals[node.b].text);
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
  // By node, whether computing it can fail.
  std::vector<bool> can_fail_;
  // By node, its kind and the register holding it.
  std::vector<Kind> kinds_;
  std::vector<uint32_t> regs_;
};

}  // namespace

base::StatusOr<PlanType> CheckExprType(const LogicalPlan& plan, const Expr& e) {
  ASSIGN_OR_RETURN(Kind kind, Typer(plan).Run(e));
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
  c.Resize(&e->nodes);
  for (ex::ExprStep& node : e->nodes) {
    c.Pod(&node.op);
    c.Pod(&node.a);
    c.Pod(&node.b);
    c.Pod(&node.c);
  }
  c.Resize(&e->literals);
  for (Literal& literal : e->literals) {
    SerializeLiteral(c, &literal);
  }
}

}  // namespace perfetto::trace_processor::pipeline
