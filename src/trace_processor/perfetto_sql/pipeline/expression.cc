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
#include <functional>
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
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/physical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_serialization.h"

namespace perfetto::trace_processor::pipeline {
namespace {

namespace ex = core::exec;
using Op = Expr::Op;
using Type = Expr::Type;

// Deeper expressions are refused, so neither compiling nor decoding one can
// run out of stack.
constexpr uint32_t kMaxDepth = 256;

// -----------------------------------------------------------------------------
// Compiling
// -----------------------------------------------------------------------------

Expr Leaf(Op op) {
  Expr e;
  e.op = op;
  return e;
}

Expr Node1(Op op, Expr a) {
  Expr e = Leaf(op);
  e.args.push_back(std::move(a));
  return e;
}

Expr Node2(Op op, Expr a, Expr b) {
  Expr e = Leaf(op);
  e.args.push_back(std::move(a));
  e.args.push_back(std::move(b));
  return e;
}

// A string literal's text: its quotes removed and doubled quotes undoubled.
std::string StringLiteral(SyntaqliteParser* p, SyntaqliteTextSpan span) {
  if (syntaqlite_span_is_quoted(span)) {
    return SpanText(p, span);
  }
  std::string text = SpanText(p, span);
  PERFETTO_DCHECK(text.size() >= 2 && text.front() == '\'');
  return base::ReplaceAll(text.substr(1, text.size() - 2), "''", "'");
}

base::StatusOr<Expr> CompileLiteral(Compiler* c,
                                    uint32_t at,
                                    const SyntaqliteLiteral& literal) {
  Expr e = Leaf(Op::kConstant);
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
      e.int_value = *value;
      return e;
    }
    case SYNTAQLITE_LITERAL_TYPE_FLOAT: {
      std::optional<double> value = base::StringToDouble(text);
      if (!value) {
        return c->Expected(at, "a number");
      }
      e.literal_type = Type::kDouble;
      e.double_value = *value;
      return e;
    }
    case SYNTAQLITE_LITERAL_TYPE_STRING:
      e.literal_type = Type::kString;
      e.string_value = StringLiteral(c->parser(), literal.source);
      return e;
    case SYNTAQLITE_LITERAL_TYPE_NULL:
      return Leaf(Op::kNull);
    default:
      return c->Unsupported(at, "this literal");
  }
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
    case SYNTAQLITE_BINARY_OP_LT:
      return Op::kLt;
    case SYNTAQLITE_BINARY_OP_GT:
      return Op::kGt;
    case SYNTAQLITE_BINARY_OP_LE:
      return Op::kLe;
    case SYNTAQLITE_BINARY_OP_GE:
      return Op::kGe;
    case SYNTAQLITE_BINARY_OP_EQ:
    case SYNTAQLITE_BINARY_OP_EQ_DOUBLE:
      return Op::kEq;
    case SYNTAQLITE_BINARY_OP_NE:
    case SYNTAQLITE_BINARY_OP_NE_ANGLE:
      return Op::kNe;
    case SYNTAQLITE_BINARY_OP_AND:
      return Op::kAnd;
    case SYNTAQLITE_BINARY_OP_OR:
      return Op::kOr;
    default:
      return std::nullopt;
  }
}

base::StatusOr<Expr> Compile(Compiler* c, uint32_t at, uint32_t depth) {
  if (depth > kMaxDepth) {
    return c->Unsupported(at, "an expression this deeply nested");
  }
  const auto* node = Node<SyntaqliteNode>(c->parser(), at);
  switch (static_cast<int>(node->tag)) {
    case SYNTAQLITE_NODE_COLUMN_REF: {
      Expr e = Leaf(Op::kColumn);
      ASSIGN_OR_RETURN(e.column, c->ResolveColumnExpr(at));
      return e;
    }
    case SYNTAQLITE_NODE_LITERAL:
      return CompileLiteral(c, at, node->literal);
    case SYNTAQLITE_NODE_PAREN_EXPR:
      return Compile(c, node->paren_expr.expr, depth + 1);
    case SYNTAQLITE_NODE_UNARY_EXPR: {
      const SyntaqliteUnaryExpr& n = node->unary_expr;
      ASSIGN_OR_RETURN(Expr a, Compile(c, n.operand, depth + 1));
      switch (n.op) {
        case SYNTAQLITE_UNARY_OP_PLUS:
          return a;
        case SYNTAQLITE_UNARY_OP_MINUS:
          // Folded, so the smallest integer can be written.
          if (a.op == Op::kConstant && a.literal_type == Type::kInt64) {
            a.int_value =
                static_cast<int64_t>(0ull - static_cast<uint64_t>(a.int_value));
            return a;
          }
          return Node1(Op::kNeg, std::move(a));
        case SYNTAQLITE_UNARY_OP_NOT:
          return Node1(Op::kNot, std::move(a));
        case SYNTAQLITE_UNARY_OP_BIT_NOT:
          return c->Unsupported(at, "bitwise operators");
      }
      PERFETTO_FATAL("For GCC");
    }
    case SYNTAQLITE_NODE_BINARY_EXPR: {
      const SyntaqliteBinaryExpr& n = node->binary_expr;
      std::optional<Op> op = BinaryOp(n.op);
      if (!op) {
        return c->Unsupported(at, "this operator");
      }
      ASSIGN_OR_RETURN(Expr a, Compile(c, n.left, depth + 1));
      ASSIGN_OR_RETURN(Expr b, Compile(c, n.right, depth + 1));
      return Node2(*op, std::move(a), std::move(b));
    }
    case SYNTAQLITE_NODE_IS_EXPR: {
      const SyntaqliteIsExpr& n = node->is_expr;
      bool is_null;
      switch (n.op) {
        case SYNTAQLITE_IS_OP_IS_NULL:
          is_null = true;
          break;
        case SYNTAQLITE_IS_OP_NOT_NULL:
        case SYNTAQLITE_IS_OP_NOT_NULL_SPACED:
          is_null = false;
          break;
        case SYNTAQLITE_IS_OP_IS:
        case SYNTAQLITE_IS_OP_IS_NOT: {
          const auto* right = Node<SyntaqliteNode>(c->parser(), n.right);
          if (right->tag != SYNTAQLITE_NODE_LITERAL ||
              right->literal.literal_type != SYNTAQLITE_LITERAL_TYPE_NULL) {
            return c->Unsupported(at, "IS with anything but NULL");
          }
          is_null = n.op == SYNTAQLITE_IS_OP_IS;
          break;
        }
        case SYNTAQLITE_IS_OP_IS_NOT_DISTINCT:
        case SYNTAQLITE_IS_OP_IS_DISTINCT:
          return c->Unsupported(at, "IS DISTINCT FROM");
      }
      ASSIGN_OR_RETURN(Expr a, Compile(c, n.left, depth + 1));
      return Node1(is_null ? Op::kIsNull : Op::kIsNotNull, std::move(a));
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

// What a subexpression computes. A bare NULL has no type of its own: it takes
// the type of whatever it meets.
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
  switch (kind) {
    case Kind::kDouble:
      return Type::kDouble;
    case Kind::kString:
      return Type::kString;
    case Kind::kInt64:
    case Kind::kNull:
    case Kind::kUnknown:
      return Type::kInt64;
  }
  PERFETTO_FATAL("For GCC");
}

const char* OpText(Op op) {
  switch (op) {
    case Op::kNeg:
    case Op::kSub:
      return "-";
    case Op::kNot:
      return "NOT";
    case Op::kAdd:
      return "+";
    case Op::kMul:
      return "*";
    case Op::kDiv:
      return "/";
    case Op::kMod:
      return "%";
    case Op::kEq:
      return "=";
    case Op::kNe:
      return "!=";
    case Op::kLt:
      return "<";
    case Op::kLe:
      return "<=";
    case Op::kGt:
      return ">";
    case Op::kGe:
      return ">=";
    case Op::kAnd:
      return "AND";
    case Op::kOr:
      return "OR";
    case Op::kColumn:
    case Op::kConstant:
    case Op::kNull:
    case Op::kToDouble:
    case Op::kIsNull:
    case Op::kIsNotNull:
      return "?";
  }
  PERFETTO_FATAL("For GCC");
}

bool IsNumber(Kind kind) {
  return kind == Kind::kInt64 || kind == Kind::kDouble;
}

// Types an expression and, given a program, appends its steps. Without one it
// only checks, as the compiler does before every column's type is known.
// An error is what was expected instead.
class Typer {
 public:
  struct Typed {
    Kind kind;
    // The register holding it. Unset for a bare NULL or without a program.
    uint32_t reg = 0;
  };

  Typer(std::function<PlanType(ColumnId)> column_type,
        std::function<uint32_t(ColumnId)> position,
        ex::ExprProgram* program,
        StringPool* pool)
      : column_type_(std::move(column_type)),
        position_(std::move(position)),
        program_(program),
        pool_(pool) {}

  base::StatusOr<Typed> Emit(const Expr& e) {
    switch (e.op) {
      case Op::kColumn: {
        Kind kind = KindOf(column_type_(e.column));
        if (kind == Kind::kNull || !program_) {
          return Typed{kind};
        }
        if (kind == Kind::kUnknown) {
          return base::ErrStatus("columns of a known type");
        }
        ex::ExprStep step;
        step.op = Op::kColumn;
        step.type = TypeOf(kind);
        step.a = position_(e.column);
        return Typed{kind, Add(step)};
      }
      case Op::kConstant: {
        Kind kind = e.literal_type == Type::kInt64    ? Kind::kInt64
                    : e.literal_type == Type::kDouble ? Kind::kDouble
                                                      : Kind::kString;
        if (!program_) {
          return Typed{kind};
        }
        ex::ExprStep step;
        step.op = Op::kConstant;
        step.type = e.literal_type;
        step.int_value = e.int_value;
        step.double_value = e.double_value;
        if (kind == Kind::kString) {
          if (!pool_) {
            return base::ErrStatus("a string pool for string literals");
          }
          step.string_value = pool_->InternString(
              base::StringView(e.string_value.data(), e.string_value.size()));
        }
        return Typed{kind, Add(step)};
      }
      case Op::kNull:
        return Typed{Kind::kNull};
      case Op::kNeg:
      case Op::kNot: {
        ASSIGN_OR_RETURN(Typed a, Emit(e.args[0]));
        bool ok = e.op == Op::kNeg
                      ? a.kind != Kind::kString
                      : a.kind != Kind::kString && a.kind != Kind::kDouble;
        if (!ok) {
          return Expected(e.op == Op::kNeg
                              ? "a number after -"
                              : "an integer truth value after NOT");
        }
        if (a.kind == Kind::kNull || a.kind == Kind::kUnknown || !program_) {
          return Typed{e.op == Op::kNot && a.kind == Kind::kUnknown
                           ? Kind::kInt64
                           : a.kind};
        }
        ex::ExprStep step;
        step.op = e.op;
        step.type = TypeOf(a.kind);
        step.operand_type = step.type;
        step.a = a.reg;
        return Typed{a.kind, Add(step)};
      }
      case Op::kAdd:
      case Op::kSub:
      case Op::kMul:
      case Op::kDiv:
      case Op::kMod:
        return Arithmetic(e);
      case Op::kEq:
      case Op::kNe:
      case Op::kLt:
      case Op::kLe:
      case Op::kGt:
      case Op::kGe:
        return Comparison(e);
      case Op::kAnd:
      case Op::kOr:
        return Logic(e);
      case Op::kIsNull:
      case Op::kIsNotNull: {
        ASSIGN_OR_RETURN(Typed a, Emit(e.args[0]));
        if (!program_) {
          return Typed{Kind::kInt64};
        }
        if (a.kind == Kind::kNull) {
          return Constant(e.op == Op::kIsNull);
        }
        ex::ExprStep step;
        step.op = e.op;
        step.a = a.reg;
        return Typed{Kind::kInt64, Add(step)};
      }
      case Op::kToDouble:
        break;
    }
    PERFETTO_FATAL("Not a logical expression");
  }

  // The register holding `t` as `kind`: a null of that type for a bare NULL,
  // an integer widened for a double.
  uint32_t Materialize(const Typed& t, Kind kind) {
    if (t.kind == Kind::kNull) {
      ex::ExprStep step;
      step.op = Op::kNull;
      step.type = TypeOf(kind);
      return Add(step);
    }
    if (t.kind == Kind::kInt64 && kind == Kind::kDouble) {
      ex::ExprStep step;
      step.op = Op::kToDouble;
      step.type = Type::kDouble;
      step.a = t.reg;
      return Add(step);
    }
    return t.reg;
  }

 private:
  static base::Status Expected(const std::string& what) {
    return base::ErrStatus("%s", what.c_str());
  }

  uint32_t Add(const ex::ExprStep& step) {
    program_->steps.push_back(step);
    return static_cast<uint32_t>(program_->steps.size() - 1);
  }

  Typed Constant(int64_t value) {
    ex::ExprStep step;
    step.op = Op::kConstant;
    step.int_value = value;
    return Typed{Kind::kInt64, Add(step)};
  }

  base::StatusOr<Typed> Arithmetic(const Expr& e) {
    ASSIGN_OR_RETURN(Typed a, Emit(e.args[0]));
    ASSIGN_OR_RETURN(Typed b, Emit(e.args[1]));
    if (a.kind == Kind::kString || b.kind == Kind::kString) {
      return Expected(std::string("numbers on both sides of ") + OpText(e.op));
    }
    if (e.op == Op::kMod &&
        (a.kind == Kind::kDouble || b.kind == Kind::kDouble)) {
      return Expected("integers on both sides of %");
    }
    if (a.kind == Kind::kUnknown || b.kind == Kind::kUnknown) {
      return Typed{Kind::kUnknown};
    }
    // Any arithmetic with NULL is NULL.
    if (a.kind == Kind::kNull || b.kind == Kind::kNull) {
      return Typed{Kind::kNull};
    }
    Kind kind = a.kind == Kind::kDouble || b.kind == Kind::kDouble
                    ? Kind::kDouble
                    : Kind::kInt64;
    if (!program_) {
      return Typed{kind};
    }
    ex::ExprStep step;
    step.op = e.op;
    step.type = TypeOf(kind);
    step.operand_type = step.type;
    step.a = Materialize(a, kind);
    step.b = Materialize(b, kind);
    return Typed{kind, Add(step)};
  }

  base::StatusOr<Typed> Comparison(const Expr& e) {
    ASSIGN_OR_RETURN(Typed a, Emit(e.args[0]));
    ASSIGN_OR_RETURN(Typed b, Emit(e.args[1]));
    bool strings = a.kind == Kind::kString || b.kind == Kind::kString;
    bool numbers = IsNumber(a.kind) || IsNumber(b.kind);
    if (strings && numbers) {
      return Expected(std::string("values of one kind on both sides of ") +
                      OpText(e.op) + ", not a number and a string");
    }
    if (strings && e.op != Op::kEq && e.op != Op::kNe) {
      return Expected(std::string("numbers on both sides of ") + OpText(e.op) +
                      ": ordering strings is not supported yet");
    }
    if (a.kind == Kind::kUnknown || b.kind == Kind::kUnknown || !program_) {
      return Typed{Kind::kInt64};
    }
    // Comparing with NULL is NULL.
    if (a.kind == Kind::kNull || b.kind == Kind::kNull) {
      return Typed{Kind::kNull};
    }
    Kind kind = strings ? Kind::kString
                : a.kind == Kind::kDouble || b.kind == Kind::kDouble
                    ? Kind::kDouble
                    : Kind::kInt64;
    ex::ExprStep step;
    step.op = e.op;
    step.type = Type::kInt64;
    step.operand_type = TypeOf(kind);
    step.a = Materialize(a, kind);
    step.b = Materialize(b, kind);
    return Typed{Kind::kInt64, Add(step)};
  }

  base::StatusOr<Typed> Logic(const Expr& e) {
    ASSIGN_OR_RETURN(Typed a, Emit(e.args[0]));
    ASSIGN_OR_RETURN(Typed b, Emit(e.args[1]));
    for (const Typed& t : {a, b}) {
      if (t.kind == Kind::kString || t.kind == Kind::kDouble) {
        return Expected(std::string("integer truth values on both sides of ") +
                        OpText(e.op));
      }
    }
    if (!program_ || a.kind == Kind::kUnknown || b.kind == Kind::kUnknown) {
      return Typed{Kind::kInt64};
    }
    ex::ExprStep step;
    step.op = e.op;
    step.a = Materialize(a, Kind::kInt64);
    step.b = Materialize(b, Kind::kInt64);
    return Typed{Kind::kInt64, Add(step)};
  }

  std::function<PlanType(ColumnId)> column_type_;
  std::function<uint32_t(ColumnId)> position_;
  ex::ExprProgram* program_;
  StringPool* pool_;
};

// -----------------------------------------------------------------------------
// Encoding
// -----------------------------------------------------------------------------

uint32_t ArgCount(Op op) {
  switch (op) {
    case Op::kColumn:
    case Op::kConstant:
    case Op::kNull:
      return 0;
    case Op::kNeg:
    case Op::kNot:
    case Op::kToDouble:
    case Op::kIsNull:
    case Op::kIsNotNull:
      return 1;
    case Op::kAdd:
    case Op::kSub:
    case Op::kMul:
    case Op::kDiv:
    case Op::kMod:
    case Op::kEq:
    case Op::kNe:
    case Op::kLt:
    case Op::kLe:
    case Op::kGt:
    case Op::kGe:
    case Op::kAnd:
    case Op::kOr:
      return 2;
  }
  PERFETTO_FATAL("For GCC");
}

Expr Read(PlanReader* c, const Available& available, uint32_t depth) {
  Reader& r = c->reader();
  Expr e;
  uint8_t op = r.U8();
  if (depth > kMaxDepth || op > static_cast<uint8_t>(Op::kIsNotNull) ||
      op == static_cast<uint8_t>(Op::kToDouble)) {
    r.Fail();
    return e;
  }
  e.op = static_cast<Op>(op);
  if (e.op == Op::kColumn) {
    e.column = r.Position(available);
  } else if (e.op == Op::kConstant) {
    uint8_t type = r.U8();
    if (type > static_cast<uint8_t>(Type::kString)) {
      r.Fail();
      return e;
    }
    e.literal_type = static_cast<Type>(type);
    if (e.literal_type == Type::kInt64) {
      e.int_value = static_cast<int64_t>(r.U64());
    } else if (e.literal_type == Type::kDouble) {
      e.double_value = r.F64();
    } else {
      e.string_value = r.Str();
    }
  }
  for (uint32_t i = 0; i < ArgCount(e.op) && r.ok(); ++i) {
    e.args.push_back(Read(c, available, depth + 1));
  }
  return e;
}

}  // namespace

base::StatusOr<Expr> CompileExpr(Compiler* c, uint32_t expr) {
  return Compile(c, expr, 0);
}

base::StatusOr<PlanType> CheckExprType(Compiler* c, const Expr& e) {
  const LogicalPlan& plan = c->plan();
  Typer typer([&](ColumnId id) { return plan.columns()[id].type; }, nullptr,
              nullptr, nullptr);
  ASSIGN_OR_RETURN(Typer::Typed t, typer.Emit(e));
  switch (t.kind) {
    case Kind::kUnknown:
      return PlanType::Unknown();
    case Kind::kDouble:
      return PlanType(core::Double{});
    case Kind::kString:
      return PlanType(core::String{});
    case Kind::kInt64:
    case Kind::kNull:
      return PlanType(core::Int64{});
  }
  PERFETTO_FATAL("For GCC");
}

void MarkExprColumns(const Expr& e, std::vector<bool>* needed) {
  if (e.op == Op::kColumn) {
    (*needed)[e.column] = true;
  }
  for (const Expr& arg : e.args) {
    MarkExprColumns(arg, needed);
  }
}

core::exec::ExprProgram LowerExpr(Lowering* c,
                                  const Expr& e,
                                  StringPool* pool,
                                  core::StorageType* type) {
  ex::ExprProgram program;
  Typer typer([c](ColumnId id) { return c->QueryType(id); },
              [c](ColumnId id) { return c->QueryPosition(id); }, &program,
              pool);
  base::StatusOr<Typer::Typed> t = typer.Emit(e);
  if (!t.ok()) {
    c->Fail(base::ErrStatus("expected %s", t.status().c_message()));
    *type = core::Int64{};
    return program;
  }
  // A result which is only ever NULL is a column of Int64 nulls.
  Kind kind = t->kind == Kind::kNull ? Kind::kInt64 : t->kind;
  typer.Materialize(*t, kind);
  if (t->kind != Kind::kNull && t->reg != program.steps.size() - 1) {
    // The result is an earlier register, such as a bare column: copy it to
    // the end so the result is the last step.
    ex::ExprStep step = program.steps[t->reg];
    program.steps.push_back(step);
  }
  if (kind == Kind::kDouble) {
    *type = core::Double{};
  } else if (kind == Kind::kString) {
    *type = core::String{};
  } else {
    *type = core::Int64{};
  }
  return program;
}

void WriteExpr(PlanWriter* c, const Expr& e, const Available& available) {
  Writer& w = c->writer();
  w.U8(static_cast<uint8_t>(e.op));
  if (e.op == Op::kColumn) {
    w.Position(available, e.column);
  } else if (e.op == Op::kConstant) {
    w.U8(static_cast<uint8_t>(e.literal_type));
    if (e.literal_type == Type::kInt64) {
      w.U64(static_cast<uint64_t>(e.int_value));
    } else if (e.literal_type == Type::kDouble) {
      w.F64(e.double_value);
    } else {
      w.Str(e.string_value);
    }
  }
  for (const Expr& arg : e.args) {
    WriteExpr(c, arg, available);
  }
}

Expr ReadExpr(PlanReader* c, const Available& available) {
  return Read(c, available, 0);
}

}  // namespace perfetto::trace_processor::pipeline
